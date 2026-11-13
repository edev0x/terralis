#ifndef FIRMWARE_IRRIGATION_H
#define FIRMWARE_IRRIGATION_H

#include <cmath>
#include <stdint.h>

#include "constants.h"
#include "utils.h"

// Balance hidrico y maquinas de estado del riego (cafe y frijol) y de la lamina
// del arroz. No depende de Arduino.
namespace Irrigation
{
    // Balance hidrico (FAO-56)
    struct Balance
    {
        float D;   // agotamiento en mm
        float TAW; // agua disponible total en mm
        float p;   // fraccion de agotamiento permitida
        float RAW() const { return p * TAW; }
    };

    // Avanza el balance un paso y lo corrige con el sensor de humedad del suelo:
    // suelo humedo -> perfil lleno; suelo seco -> al menos el agotamiento permitido.
    inline void update_balance(Balance &b, float etc_mm, float eff_rain_mm, float irrigation_mm,
                               float soil_pct, float low_humidity, float high_humidity)
    {
        b.D = Utils::clampf(b.D + etc_mm - eff_rain_mm - irrigation_mm, 0.0f, b.TAW);
        if (std::isnan(soil_pct))
            return;
        if (soil_pct >= high_humidity)
            b.D = 0.0f;
        else if (soil_pct <= low_humidity && b.D < b.RAW())
            b.D = b.RAW();
    }

    // Linea de riego (cafe, frijol)
    struct LineParams
    {
        float low_humidity;  // % que inicia el riego
        float high_humidity; // % que lo detiene
        float max_s;         // duracion maxima de un riego
        float rest_s;        // reposo obligatorio despues de regar
    };

    struct Line
    {
        uint8_t state; // Constants::IrrigationState
        float t_s;     // tiempo en el estado actual

        void init()
        {
            state = Constants::IR_IN_RANGE;
            t_s = 0.0f;
        }
    };

    // true si la linea necesita la bomba en este ciclo
    inline bool line_demand(const Line &line, const LineParams &p, float soil_pct, float D, float RAW,
                            bool enabled, bool blocked)
    {
        if (!enabled || blocked)
            return false;
        if (line.state == Constants::IR_IRRIGATING)
            return true;
        if (line.state != Constants::IR_IN_RANGE)
            return false;
        return soil_pct < p.low_humidity || D >= RAW;
    }

    // Urgencia del riego: >= 1 cuando se cruza alguno de los dos umbrales
    inline float line_score(const LineParams &p, float soil_pct, float D, float RAW)
    {
        float by_balance = RAW > 0.0f ? D / RAW : 0.0f;
        float by_sensor = (p.low_humidity > 0.0f && !std::isnan(soil_pct))
                              ? 1.0f + (p.low_humidity - soil_pct) / p.low_humidity
                              : 0.0f;
        return by_balance > by_sensor ? by_balance : by_sensor;
    }

    // EN_RANGO -> REGANDO (con turno) -> REPOSO (suelo humedo o tiempo maximo) -> EN_RANGO
    inline void line_step(Line &line, const LineParams &p, float soil_pct, bool enabled, bool blocked,
                          bool turn, float dt_s)
    {
        switch (line.state)
        {
        case Constants::IR_IN_RANGE:
            if (turn && enabled && !blocked)
            {
                line.state = Constants::IR_IRRIGATING;
                line.t_s = 0.0f;
            }
            break;
        case Constants::IR_IRRIGATING:
            line.t_s += dt_s;
            if (!enabled || blocked)
            {
                line.state = Constants::IR_IN_RANGE; // riego interrumpido: sin reposo
                line.t_s = 0.0f;
            }
            else if (soil_pct >= p.high_humidity || line.t_s >= p.max_s)
            {
                line.state = Constants::IR_IDLE;
                line.t_s = 0.0f;
            }
            break;
        default: // IR_IDLE
            line.t_s += dt_s;
            if (line.t_s >= p.rest_s)
            {
                line.state = Constants::IR_IN_RANGE;
                line.t_s = 0.0f;
            }
            break;
        }
    }

    // Lamina del arroz
    struct LayerParams
    {
        float target; // lamina objetivo (cm)
        float band;   // banda +/- alrededor del objetivo
        float margin; // exceso sobre el objetivo que abre el drenaje
    };

    // Drenaje final: empieza por encima de START y termina al llegar a EMPTY
    constexpr float FINAL_DRAIN_START_CM = 0.5f;
    constexpr float FINAL_DRAIN_EMPTY_CM = 0.1f;

    inline bool layer_demand(uint8_t state, float layer, const LayerParams &lp, bool fill_enabled,
                             bool blocked)
    {
        if (!fill_enabled || blocked)
            return false;
        if (state == Constants::LAYER_FILLING)
            return layer < lp.target;
        return state == Constants::LAYER_IN_BAND && layer < lp.target - lp.band;
    }

    // Urgencia del llenado: >= 1 cuando la lamina sale de la banda por debajo
    inline float layer_score(float layer, const LayerParams &lp)
    {
        return lp.band > 0.0f ? (lp.target - layer) / lp.band : 0.0f;
    }

    // EN_BANDA -> LLENANDO (con turno, hasta el objetivo)
    // EN_BANDA -> DRENANDO (exceso de lamina, o drenaje final hasta vaciar)
    inline void layer_step(uint8_t &state, float layer, const LayerParams &lp, bool final_drain,
                           bool fill_enabled, bool blocked, bool turn)
    {
        switch (state)
        {
        case Constants::LAYER_IN_BAND:
            if (final_drain ? layer > FINAL_DRAIN_START_CM : layer > lp.target + lp.margin)
                state = Constants::LAYER_DRAINING;
            else if (turn && fill_enabled && !blocked)
                state = Constants::LAYER_FILLING;
            break;
        case Constants::LAYER_FILLING:
            if (!fill_enabled || blocked || layer >= lp.target)
                state = Constants::LAYER_IN_BAND;
            break;
        default: // LAYER_DRAINING
            if (final_drain ? layer <= FINAL_DRAIN_EMPTY_CM : layer <= lp.target)
                state = Constants::LAYER_IN_BAND;
            break;
        }
    }

    // Lamina calculada cuando no hay sensor: entradas - salidas en el intervalo dt
    inline float layer_model(float layer, float dt_s, bool filling, bool draining, float etc_mm_day,
                             float rain_mm, float inflow_cm_h, float drain_cm_h, float perc_mm_day)
    {
        float hours = dt_s / 3600.0f, days = dt_s / 86400.0f;
        if (filling)
            layer += inflow_cm_h * hours;
        if (draining)
            layer -= drain_cm_h * hours;
        layer -= (etc_mm_day + perc_mm_day) * days / 10.0f; // mm -> cm
        layer += rain_mm / 10.0f;
        return layer < 0.0f ? 0.0f : layer;
    }

    // Turno de la bomba
    // Una sola linea a la vez: si alguna ya esta activa no se abre otra. Si no,
    // gana la de mayor urgencia entre las que piden agua. Devuelve -1 si no hay turno.
    inline int pick_line(bool any_active, const bool *demand, const float *score, int n)
    {
        if (any_active)
            return -1;
        int best = -1;
        for (int i = 0; i < n; i++)
            if (demand[i] && (best < 0 || score[i] > score[best]))
                best = i;
        return best;
    }
}

#endif