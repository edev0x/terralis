#ifndef FIRMWARE_CONTROL_H
#define FIRMWARE_CONTROL_H

#include <cmath>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "constants.h"
#include "irrigation.h"
#include "pins.h"
#include "utils.h"

// Logica de un ciclo de control: balance hidrico, bloqueos, turno de la bomba,
// maquinas de estado, riesgo fitosanitario y salidas.
namespace Control
{
    struct Input
    {
        uint32_t ms;      // millis() al empezar el ciclo
        uint32_t epoch;   // tiempo simulado
        float rain_mm;    // lluvia recibida desde el ciclo anterior
        bool has_weather; // llego una medida nueva de clima
        Utils::WeatherMeasure weather;
        bool has_water;   // llego una medida nueva de agua
        Utils::WaterMeasure water;
        uint8_t stage[Constants::N_CROPS]; // etapa elegida por el usuario
    };

    struct Output
    {
        bool ready;      // false: todavia no hay datos, todo queda cerrado
        uint8_t outputs; // bits del 74HC595 (Pins::Out)
        uint8_t alerts;  // alertas nuevas: bit n = Constants::CodeAlert n
        bool alarm;      // hay condicion de alarm
        Utils::Snapshot snap;
    };

    class Controller
    {
    public:
        void init(uint32_t ms)
        {
            rain.init();
            trend.init();
            for (int i = 0; i < Constants::N_CROPS; i++)
                risk[i].init(Config::Risk::MEDIUM_HOURS[i], Config::Risk::HIGH_HOURS[i]);

            coffeeBalance.D = 0.0f;
            coffeeBalance.TAW = Config::Coffee::TAW_MM;
            coffeeBalance.p = Config::Coffee::DEPLETION_FRACTION;
            beanBalance.D = 0.0f;
            beanBalance.TAW = Config::Bean::TAW_MM;
            beanBalance.p = Config::Bean::DEPLETION_FRACTION;
            coffeeLine.init();
            beanLine.init();
            riceState = Constants::LAYER_IN_BAND;
            layer = Config::Rice::LAYER_INITIAL_CM;

            weather.temp = weather.humidity = weather.pressure = weather.rad = NAN;
            weather.leaf = 0.0f;
            weather.ok = false;
            water.soil_coffee = water.soil_bean = 0.0f;
            water.layer = water.reservoir = NAN;
            water.layer_ok = water.tank_ok = true;
            hasWeather = hasWater = false;
            badWeather = badLayer = badTank = 0;
            lastWeatherMs = lastWaterMs = prevMs = ms;

            et0 = Config::Climate::ET0_INITIAL;
            tMaxDay = -100.0f;
            tMinDay = 100.0f;
            todayRain = 0.0f;
            pendingEffRain = 0.0f;
            currentDay = -1;
            reservoirLocked = false;

            prevBlockages = 0;
            prevSafe = false;
            for (int i = 0; i < Constants::N_CROPS; i++)
                prevLevel[i] = 0;
        }

        // Ejecuta un ciclo de control
        void step(const Input &in, Output &out)
        {
            Cycle c = Cycle();
            out = Output();

            readInputs(in, c);
            dayChange(c);
            processWeather(c);
            detectFailures(c);

            // Sin datos y sin falla: se espera con todo cerrado
            if (!(hasWeather && hasWater) && !c.safe)
                return;

            loadStages(in, c);
            updateLayer(c);
            updateBalances(c);
            computeBlockages(c);
            runStateMachines(c);
            evaluateAlerts(c, out);
            computeOutputs(c);
            fillSnapshot(c, out.snap);

            out.ready = true;
            out.outputs = c.outputs;
            savePrevious(c);
        }

    private:
        struct Cycle
        {
            uint32_t ms, epoch;
            float dt; // segundos simulados desde el ciclo anterior
            Time::Date date;
            float rain_mm;
            bool newWeather;
            uint8_t failures;
            bool safe; // modo seguro: hay alguna falla de sensores
            uint8_t stage[Constants::N_CROPS];
            uint8_t season;
            Irrigation::LayerParams layerParams;
            bool finalDrain, fillEnabled, coffeeEnabled, beanEnabled;
            float rain6h;
            uint8_t blockages;
            bool blocked;
            uint8_t level[Constants::N_CROPS];
            bool anyHigh;
            uint8_t outputs;
        };

        static const int MAX_DT_PERIODS = 10;

        static Irrigation::LineParams coffeeParams()
        {
            Irrigation::LineParams p = {Config::Coffee::LOW_HUMIDITY, Config::Coffee::HIGH_HUMIDITY,
                                        Config::Coffee::MAX_IRRIGATION_S, Config::Coffee::REST_S};
            return p;
        }

        static Irrigation::LineParams beanParams()
        {
            Irrigation::LineParams p = {Config::Bean::LOW_HUMIDITY, Config::Bean::HIGH_HUMIDITY,
                                        Config::Bean::MAX_IRRIGATION_S, Config::Bean::REST_S};
            return p;
        }

        static uint8_t bitIf(bool on, uint8_t bit) { return on ? (uint8_t)(1u << bit) : 0; }

        // 1. Tiempo y datos nuevos
        void readInputs(const Input &in, Cycle &c)
        {
            c.ms = in.ms;
            c.epoch = in.epoch;
            c.date = Time::from_epoch(in.epoch);

            // dt se limita para que un atasco no integre un paso gigante
            const float accel = (float)Config::TIME_ACCEL;
            const float dtMax = MAX_DT_PERIODS * Config::Timing::CONTROL_PERIOD_MS * accel / 1000.0f;
            c.dt = (in.ms - prevMs) * accel / 1000.0f;
            if (c.dt > dtMax)
                c.dt = dtMax;
            prevMs = in.ms;

            c.rain_mm = in.rain_mm;
            if (in.rain_mm > 0.0f)
            {
                rain.add(in.rain_mm, in.epoch);
                todayRain += in.rain_mm;
            }

            c.newWeather = in.has_weather;
            if (in.has_weather)
            {
                weather = in.weather;
                hasWeather = true;
                lastWeatherMs = in.ms;
                badWeather = in.weather.ok ? 0 : badWeather + 1;
            }

            if (in.has_water)
            {
                water = in.water;
                hasWater = true;
                lastWaterMs = in.ms;
                badLayer = in.water.layer_ok ? 0 : badLayer + 1;
                badTank = in.water.tank_ok ? 0 : badTank + 1;
            }
        }

        // 2. Cambio de dia: ET0 de Hargreaves y lluvia efectiva
        void dayChange(const Cycle &c)
        {
            if (currentDay == -1)
            {
                currentDay = c.date.day;
                return;
            }
            if (c.date.day == currentDay)
                return;

            if (tMaxDay > tMinDay)
            {
                // Las temperaturas son del dia que termino: se usa su fecha
                Time::Date y = Time::from_epoch(c.epoch - 86400UL);
                float ra = Utils::rad_mm_day(Config::Site::LATITUDE, Time::day_of_year(y.year, y.month, y.day));
                et0 = Utils::et0_hargreaves(0.5f * (tMaxDay + tMinDay), tMaxDay, tMinDay, ra);
            }
            pendingEffRain += Utils::effective_rain(todayRain); // la aplica updateBalances()
            todayRain = 0.0f;
            tMaxDay = -100.0f;
            tMinDay = 100.0f;
            currentDay = c.date.day;
        }

        // 3. Clima: temperaturas del dia, presion y riesgo fitosanitario
        void processWeather(const Cycle &c)
        {
            if (!(c.newWeather && weather.ok))
                return;

            if (weather.temp > tMaxDay)
                tMaxDay = weather.temp;
            if (weather.temp < tMinDay)
                tMinDay = weather.temp;
            trend.sample(weather.pressure, c.epoch);

            bool wetLeaf = weather.leaf >= Config::Sensors::WET_LEAF_PCT;
            for (int i = 0; i < Constants::N_CROPS; i++)
            {
                bool humid = (i == Constants::CROP_COFFEE) ? wetLeaf
                                                           : weather.humidity >= Config::Risk::RH_MIN[i];
                bool favorable = weather.temp >= Config::Risk::T_MIN[i] &&
                                 weather.temp <= Config::Risk::T_MAX[i] && humid;
                risk[i].sample(favorable, c.epoch / 3600u);
            }
        }

        // 4. Fallas de sensores: lecturas malas seguidas o datos vencidos
        void detectFailures(Cycle &c)
        {
            const int MAX_BAD = Config::Sensors::BAD_READINGS_MAX;
            bool oldWeather = (c.ms - lastWeatherMs) > Config::Timing::staleAfter(Config::Timing::WEATHER_PERIOD_MS);
            bool oldWater = (c.ms - lastWaterMs) > Config::Timing::staleAfter(Config::Timing::WATER_PERIOD_MS);

            c.failures = 0;
            if (badWeather >= MAX_BAD || oldWeather)
                c.failures |= Constants::FL_WEATHER;
            if (!Config::Rice::LAYER_MODELED && (badLayer >= MAX_BAD || oldWater))
                c.failures |= Constants::FL_LAYER;
            if (badTank >= MAX_BAD || oldWater)
                c.failures |= Constants::FL_TANK;
            c.safe = c.failures != 0;
        }

        // 5. Etapas y parametros que dependen de ellas
        void loadStages(const Input &in, Cycle &c)
        {
            for (int i = 0; i < Constants::N_CROPS; i++)
                c.stage[i] = in.stage[i] < Config::stage_count(i) ? in.stage[i] : 0;

            uint8_t rice = c.stage[Constants::CROP_RICE];
            c.layerParams.target = Config::Rice::LAYER_TARGET_CM[rice];
            c.layerParams.band = Config::Rice::LAYER_BAND_CM;
            c.layerParams.margin = Config::Rice::LAYER_MAX_MARGIN_CM;
            c.finalDrain = (rice == Config::Rice::DRAIN_STAGE);
            c.fillEnabled = Config::Rice::IRRIGATED && !c.finalDrain;

            c.season = Constants::get_season(c.date.month, c.date.day);
            c.coffeeEnabled = (c.season == Constants::DRY); // el cafe solo se riega en epoca seca
            c.beanEnabled = true;
        }

        // 6. Lamina del arroz (modelo o sensor)
        void updateLayer(const Cycle &c)
        {
            if (Config::Rice::LAYER_MODELED)
            {
                float etc = Config::Rice::KC[c.stage[Constants::CROP_RICE]] * et0;
                layer = Irrigation::layer_model(layer, c.dt,
                                                riceState == Constants::LAYER_FILLING && !c.safe,
                                                riceState == Constants::LAYER_DRAINING && !c.safe, etc,
                                                c.rain_mm, Config::Rice::LAYER_INFLOW_CM_H,
                                                Config::Rice::LAYER_DRAIN_CM_H,
                                                Config::Rice::LAYER_PERCOLATION_MM_DAY);
            }
            else if (water.layer_ok && !std::isnan(water.layer))
            {
                layer = water.layer;
            }
        }

        // 7. Balance hidrico de cafe y frijol
        void updateBalances(const Cycle &c)
        {
            const float days = c.dt / 86400.0f, hours = c.dt / 3600.0f;
            bool coffeeOn = coffeeLine.state == Constants::IR_IRRIGATING && !c.safe;
            bool beanOn = beanLine.state == Constants::IR_IRRIGATING && !c.safe;

            Irrigation::update_balance(coffeeBalance,
                                       Config::Coffee::KC[c.stage[Constants::CROP_COFFEE]] * et0 * days,
                                       pendingEffRain,
                                       coffeeOn ? Config::Coffee::IRRIGATION_RATE_MM_H * hours : 0.0f,
                                       water.soil_coffee, Config::Coffee::LOW_HUMIDITY,
                                       Config::Coffee::HIGH_HUMIDITY);
            Irrigation::update_balance(beanBalance,
                                       Config::Bean::KC[c.stage[Constants::CROP_BEAN]] * et0 * days,
                                       pendingEffRain,
                                       beanOn ? Config::Bean::IRRIGATION_RATE_MM_H * hours : 0.0f,
                                       water.soil_bean, Config::Bean::LOW_HUMIDITY,
                                       Config::Bean::HIGH_HUMIDITY);
            pendingEffRain = 0.0f;
        }

        // 8. Bloqueos: lluvia reciente, tormenta proxima, reservorio bajo o falla
        void computeBlockages(Cycle &c)
        {
            c.rain6h = rain.lasts(6, c.epoch);
            float pressureFall = weather.ok ? trend.fall(weather.pressure) : 0.0f;

            if (water.tank_ok)
            {
                if (water.reservoir < Config::Blocking::RESERVOIR_LOW_PCT)
                    reservoirLocked = true;
                else if (water.reservoir >= Config::Blocking::RESERVOIR_OK_PCT)
                    reservoirLocked = false; // histeresis
            }

            c.blockages = 0;
            if (c.rain6h >= Config::Blocking::RAIN_6H_MM)
                c.blockages |= Constants::BL_RAIN;
            if (pressureFall >= Config::Blocking::PRESSURE_DROP_HPA)
                c.blockages |= Constants::BL_PRESSURE;
            if (reservoirLocked)
                c.blockages |= Constants::BL_RESERVOIR;
            if (c.safe)
                c.blockages |= Constants::BL_FAILURE;
            c.blocked = c.blockages != 0;
        }

        // 9. Turno de la bomba (una sola linea a la vez) y maquinas de estado
        void runStateMachines(const Cycle &c)
        {
            const Irrigation::LineParams cp = coffeeParams(), bp = beanParams();
            bool demand[Constants::N_CROPS];
            float score[Constants::N_CROPS];

            demand[0] = Irrigation::line_demand(coffeeLine, cp, water.soil_coffee, coffeeBalance.D,
                                                coffeeBalance.RAW(), c.coffeeEnabled, c.blocked);
            demand[1] = Irrigation::line_demand(beanLine, bp, water.soil_bean, beanBalance.D,
                                                beanBalance.RAW(), c.beanEnabled, c.blocked);
            demand[2] = Irrigation::layer_demand(riceState, layer, c.layerParams, c.fillEnabled, c.blocked);
            score[0] = Irrigation::line_score(cp, water.soil_coffee, coffeeBalance.D, coffeeBalance.RAW());
            score[1] = Irrigation::line_score(bp, water.soil_bean, beanBalance.D, beanBalance.RAW());
            score[2] = Irrigation::layer_score(layer, c.layerParams);

            bool anyActive = coffeeLine.state == Constants::IR_IRRIGATING ||
                             beanLine.state == Constants::IR_IRRIGATING ||
                             riceState == Constants::LAYER_FILLING;
            int turn = Irrigation::pick_line(anyActive, demand, score, Constants::N_CROPS);

            Irrigation::line_step(coffeeLine, cp, water.soil_coffee, c.coffeeEnabled, c.blocked,
                                  turn == Constants::CROP_COFFEE, c.dt);
            Irrigation::line_step(beanLine, bp, water.soil_bean, c.beanEnabled, c.blocked,
                                  turn == Constants::CROP_BEAN, c.dt);
            Irrigation::layer_step(riceState, layer, c.layerParams, c.finalDrain, c.fillEnabled,
                                   c.blocked, turn == Constants::CROP_RICE);
        }

        // 10. Riesgo y alertas
        void evaluateAlerts(Cycle &c, Output &out)
        {
            c.anyHigh = false;
            for (int i = 0; i < Constants::N_CROPS; i++)
            {
                c.level[i] = risk[i].level();
                if (c.level[i] == 2)
                {
                    c.anyHigh = true;
                    if (prevLevel[i] != 2)
                        out.alerts |= 1u << (Constants::AL_COFFEE_RISK + i);
                }
            }
            if ((c.blockages & Constants::BL_RESERVOIR) && !(prevBlockages & Constants::BL_RESERVOIR))
                out.alerts |= 1u << Constants::AL_RESERVOIR;
            if ((c.blockages & Constants::BL_PRESSURE) && !(prevBlockages & Constants::BL_PRESSURE))
                out.alerts |= 1u << Constants::AL_PRESSURE;
            if (c.safe && !prevSafe)
                out.alerts |= 1u << Constants::AL_FAILURE;

            out.alarm = c.anyHigh || c.safe || (c.blockages & Constants::BL_RESERVOIR);
        }

        // 11. Salidas. En modo seguro todas las valvulas quedan cerradas.
        void computeOutputs(Cycle &c)
        {
            bool vCoffee = coffeeLine.state == Constants::IR_IRRIGATING && !c.safe;
            bool vBean = beanLine.state == Constants::IR_IRRIGATING && !c.safe;
            bool vRiceIn = riceState == Constants::LAYER_FILLING && !c.safe;
            bool vRiceOut = riceState == Constants::LAYER_DRAINING && !c.safe;
            bool pump = vCoffee || vBean || vRiceIn;

            bool anyMedium = c.level[0] == 1 || c.level[1] == 1 || c.level[2] == 1;
            bool red = c.safe || (c.blockages & Constants::BL_RESERVOIR) || c.anyHigh;
            bool yellow = !red && ((c.blockages & (Constants::BL_RAIN | Constants::BL_PRESSURE)) || anyMedium);
            bool green = !red && !yellow;

            c.outputs = bitIf(pump, Pins::Out::PUMP) | bitIf(vCoffee, Pins::Out::V_COFFEE) |
                        bitIf(vBean, Pins::Out::V_BEAN) | bitIf(vRiceIn, Pins::Out::V_RICE_IN) |
                        bitIf(vRiceOut, Pins::Out::V_RICE_OUT) | bitIf(green, Pins::Out::LED_GREEN) |
                        bitIf(yellow, Pins::Out::LED_YELLOW) | bitIf(red, Pins::Out::LED_RED);
        }

        // 12. Informacion para las demas tareas
        void fillSnapshot(const Cycle &c, Utils::Snapshot &s)
        {
            memset(&s, 0, sizeof(s));
            s.epoch = c.epoch;
            s.t = weather.temp;
            s.hr = weather.humidity;
            s.p = weather.pressure;
            s.rad = weather.rad;
            s.rain_day = rain.day(c.epoch);
            s.rain_6h = c.rain6h;
            s.coffee_soil = water.soil_coffee;
            s.bean_soil = water.soil_bean;
            s.leaf = (weather.leaf >= Config::Sensors::WET_LEAF_PCT) ? 1 : 0;
            s.layer = layer;
            s.layer_target = c.layerParams.target;
            s.reservoir = water.reservoir;
            s.state_coffee = coffeeLine.state;
            s.state_bean = beanLine.state;
            s.state_rice = riceState;
            for (int i = 0; i < Constants::N_CROPS; i++)
            {
                s.risk[i] = c.level[i];
                s.h48[i] = (uint8_t)risk[i].h48();
                s.stage[i] = c.stage[i];
            }
            s.season = c.season;
            s.blockages = c.blockages;
            s.failures = c.failures;
            s.safe = c.safe ? 1 : 0;
            s.outputs = c.outputs;
            s.et0 = et0;
            s.d_coffee = coffeeBalance.D;
            s.d_bean = beanBalance.D;
        }

        void savePrevious(const Cycle &c)
        {
            prevBlockages = c.blockages;
            prevSafe = c.safe;
            for (int i = 0; i < Constants::N_CROPS; i++)
                prevLevel[i] = c.level[i];
        }

        // Estado persistente
        Utils::RainTracker rain;
        Utils::PressureTrend trend;
        Utils::FungalRisk risk[Constants::N_CROPS];

        Irrigation::Balance coffeeBalance, beanBalance;
        Irrigation::Line coffeeLine, beanLine;
        uint8_t riceState;
        float layer; // lamina del arroz (cm)

        Utils::WeatherMeasure weather; // ultima medida recibida
        Utils::WaterMeasure water;
        bool hasWeather, hasWater;
        int badWeather, badLayer, badTank; // lecturas malas seguidas
        uint32_t lastWeatherMs, lastWaterMs, prevMs;

        float et0;
        float tMaxDay, tMinDay;
        float todayRain;
        float pendingEffRain; // lluvia efectiva aun no aplicada al balance
        int currentDay;
        bool reservoirLocked;

        // Valores del ciclo anterior, para detectar cambios
        uint8_t prevBlockages;
        bool prevSafe;
        uint8_t prevLevel[Constants::N_CROPS];
    };
}

#endif