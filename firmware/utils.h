#ifndef FIRMWARE_UTILS_H
#define FIRMWARE_UTILS_H

#include <cmath>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

// Fechas, calculos agrometeorologicos, acumuladores y mensajes entre tareas.
// No depende de Arduino: se puede compilar y probar en una PC.

// Fechas a partir del epoch Unix
namespace Time
{
    struct Date
    {
        int year, month, day, hh, mi, ss;
    };

    inline Date from_epoch(uint32_t epoch)
    {
        uint32_t days = epoch / 86400u, sec = epoch % 86400u;
        int64_t z = (int64_t)days + 719468;
        int64_t era = (z >= 0 ? z : z - 146096) / 146097;

        uint32_t doe = (uint32_t)(z - era * 146097);
        uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;

        int64_t y = (int64_t)yoe + era * 400;
        uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        uint32_t mp = (5 * doy + 2) / 153;
        uint32_t d = doy - (153 * mp + 2) / 5 + 1;
        uint32_t m = mp < 10 ? mp + 3 : mp - 9;

        if (m <= 2)
            y += 1;

        Date date;
        date.year = (int)y;
        date.month = (int)m;
        date.day = (int)d;
        date.hh = (int)(sec / 3600);
        date.mi = (int)((sec % 3600) / 60);
        date.ss = (int)(sec % 60);
        return date;
    }

    inline int day_of_year(int y, int m, int d)
    {
        static const int acum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        int n = acum[m - 1] + d;
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));

        if (leap && m > 2)
            n++;
        return n;
    }

    // "AAAA-MM-DD hh:mm:ss"; el buffer debe tener al menos 20 caracteres
    inline void date_text(uint32_t epoch, char *buf, size_t n)
    {
        Date date = from_epoch(epoch);
        snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d", date.year, date.month, date.day,
                 date.hh, date.mi, date.ss);
    }
}

namespace Utils
{
    struct WeatherMeasure
    {
        float temp, humidity, pressure; // C, %, hPa
        float rad, leaf;                // luz y humedad de hoja en %
        bool ok;
    };

    struct WaterMeasure
    {
        float soil_coffee, soil_bean; // % de humedad
        float layer;                  // lamina del arroz (cm)
        float reservoir;              // %
        bool layer_ok, tank_ok;
    };

    struct Alert
    {
        uint8_t code;   // Constants::CodeAlert
        uint32_t epoch;
    };

    // Estado completo del sistema en un ciclo de control (pantalla, SD y MQTT)
    struct Snapshot
    {
        uint32_t epoch;
        float t, hr, p, rad; // temperatura, humedad relativa, presion, luz
        float rain_day, rain_6h;
        float coffee_soil, bean_soil;
        uint8_t leaf;                          // 1 = hoja mojada
        float layer, layer_target, reservoir;  // lamina actual y objetivo (cm), reservorio (%)
        uint8_t state_coffee, state_bean, state_rice;
        uint8_t risk[3];  // 0 = bajo, 1 = medio, 2 = alto
        uint8_t h48[3];   // horas favorables en las ultimas 48 h
        uint8_t stage[3]; // etapa fenologica de cada cultivo
        uint8_t season;
        uint8_t blockages, failures, safe, outputs;
        float et0, d_coffee, d_bean;
    };

    constexpr float PI_F = 3.14159265f;

    inline float clampf(float value, float low, float high)
    {
        return value < low ? low : (value > high ? high : value);
    }

    // Ordena v y devuelve la mediana
    inline float median(float *v, int n)
    {
        for (int i = 1; i < n; i++)
        {
            float x = v[i];
            int j = i - 1;
            while (j >= 0 && v[j] > x)
            {
                v[j + 1] = v[j];
                j--;
            }
            v[j + 1] = x;
        }
        return v[n / 2];
    }

    // Escribe un numero, o el texto 'empty' si el valor no es valido
    inline void num_txt(char *buf, size_t n, float v, int dec, const char *empty)
    {
        if (std::isnan(v) || std::isinf(v))
            snprintf(buf, n, "%s", empty);
        else
            snprintf(buf, n, "%.*f", dec, v);
    }

    // Evapotranspiracion y lluvia
    // Radiacion extraterrestre en mm/dia equivalentes (FAO-56)
    inline float rad_mm_day(float lat_deg, int doy)
    {
        const float GSC = 0.0820f; // constante solar (MJ m^-2 min^-1)

        float phi = lat_deg * PI_F / 180.0f;                            // latitud en radianes
        float dr = 1.0f + 0.033f * cosf(2.0f * PI_F / 365.0f * doy);    // distancia relativa Tierra-Sol
        float delta = 0.409f * sinf(2.0f * PI_F / 365.0f * doy - 1.39f); // declinacion solar

        float ws = acosf(clampf(-tanf(phi) * tanf(delta), -1.0f, 1.0f));
        float ra = (24.0f * 60.0f / PI_F) * GSC * dr *
                   (ws * sinf(phi) * sinf(delta) + cosf(phi) * cosf(delta) * sinf(ws));

        return ra / 2.45f; // MJ m^-2 dia^-1 -> mm/dia
    }

    // ET0 de Hargreaves en mm/dia
    inline float et0_hargreaves(float tmed, float tmax, float tmin, float ra_mm)
    {
        float dt = tmax - tmin;
        if (dt < 0.0f)
            dt = 0.0f;

        float v = 0.0023f * ra_mm * (tmed + 17.8f) * sqrtf(dt);
        return v < 0.0f ? 0.0f : v;
    }

    // Lluvia efectiva a partir del total del dia
    inline float effective_rain(float p_mm)
    {
        return p_mm < 5.0f ? 0.0f : 0.8f * p_mm;
    }

    // Tendencia de presion
    // Guarda una muestra cada 15 minutos (13 muestras = 3 horas)
    struct PressureTrend
    {
        float v[13];
        uint8_t n;
        uint32_t last;

        void init()
        {
            n = 0;
            last = 0;
        }

        void sample(float p, uint32_t epoch)
        {
            if (n != 0 && epoch >= last && epoch - last < 900u)
                return;

            if (n < 13)
            {
                v[n++] = p;
            }
            else
            {
                for (int i = 0; i < 12; i++)
                    v[i] = v[i + 1];
                v[12] = p;
            }
            last = epoch;
        }

        // Caida respecto al maximo de las ultimas 3 horas (hPa)
        float fall(float p_now) const
        {
            float mx = p_now;
            for (int i = 0; i < n; i++)
                if (v[i] > mx)
                    mx = v[i];
            return mx - p_now;
        }
    };

    struct RainTracker
    {
        float hours[24];
        uint32_t act_hour;
        float day_mm;
        uint32_t act_day;
        bool started;

        void init()
        {
            for (int i = 0; i < 24; i++)
                hours[i] = 0.0f;
            act_hour = 0;
            day_mm = 0.0f;
            act_day = 0;
            started = false;
        }

        void moveForward(uint32_t epoch)
        {
            uint32_t h = epoch / 3600u, d = epoch / 86400u;
            if (!started)
            {
                act_hour = h;
                act_day = d;
                started = true;
            }
            if (h != act_hour)
            {
                if (h < act_hour || h - act_hour >= 24)
                {
                    for (int i = 0; i < 24; i++)
                        hours[i] = 0.0f;
                }
                else
                {
                    for (uint32_t x = act_hour + 1; x <= h; x++)
                        hours[x % 24] = 0.0f;
                }
                act_hour = h;
            }
            if (d != act_day)
            {
                day_mm = 0.0f;
                act_day = d;
            }
        }

        void add(float mm, uint32_t epoch)
        {
            moveForward(epoch);
            hours[act_hour % 24] += mm;
            day_mm += mm;
        }

        // Lluvia de las ultimas n horas (incluye la hora en curso)
        float lasts(int n, uint32_t epoch)
        {
            moveForward(epoch);
            float s = 0.0f;
            for (int i = 0; i < n && i < 24; i++)
                s += hours[(act_hour + 24 - i) % 24];
            return s;
        }

        float day(uint32_t epoch)
        {
            moveForward(epoch);
            return day_mm;
        }
    };

    // Riesgo fitosanitario
    // Cuenta las horas con condiciones favorables para el hongo en las ultimas 48 h.
    // Una hora es favorable si lo fueron al menos la mitad de sus muestras.
    struct FungalRisk
    {
        uint8_t flags[Config::Risk::SLOTS];
        uint32_t currentHour;
        uint16_t favorable, total;
        int medium, high;
        bool started;

        void init(int mediumHours, int highHours)
        {
            memset(flags, 0, sizeof(flags));
            currentHour = 0;
            favorable = 0;
            total = 0;
            medium = mediumHours;
            high = highHours;
            started = false;
        }

        void closeHour(uint32_t newHour)
        {
            const uint32_t SLOTS = Config::Risk::SLOTS;
            uint8_t flag = (total > 0 && (uint32_t)favorable * 2u >= total) ? 1 : 0;
            flags[currentHour % SLOTS] = flag;
            if (newHour < currentHour || newHour - currentHour > SLOTS)
            {
                memset(flags, 0, sizeof(flags));
            }
            else
            {
                for (uint32_t h = currentHour + 1; h < newHour; h++)
                    flags[h % SLOTS] = 0;
            }
            flags[newHour % SLOTS] = 0;
            currentHour = newHour;
            favorable = 0;
            total = 0;
        }

        // hour = epoch / 3600 de la muestra
        void sample(bool favorableSample, uint32_t hour)
        {
            if (!started)
            {
                currentHour = hour;
                started = true;
            }
            else if (hour != currentHour)
            {
                closeHour(hour);
            }
            total++;
            if (favorableSample)
                favorable++;
        }

        // Horas favorables entre las ultimas 48 horas cerradas
        int h48() const
        {
            const uint32_t SLOTS = Config::Risk::SLOTS;
            int count = 0;
            for (uint32_t i = 0; i < SLOTS; i++)
                if (i != currentHour % SLOTS)
                    count += flags[i];
            return count;
        }

        // 0 = bajo, 1 = medio, 2 = alto
        uint8_t level() const
        {
            int hours = h48();
            if (hours >= high)
                return 2;
            if (hours >= medium)
                return 1;
            return 0;
        }
    };
}

#endif // FIRMWARE_UTILS_H