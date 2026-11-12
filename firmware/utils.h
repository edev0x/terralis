#ifndef FIRMWARE_UTILS_H
#define FIRMWARE_UTILS_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "time.h"

class Utils
{
public:
    Utils() = delete;

    struct LayerParam {
        float target; // lamina objetivo cm
        float strip; // banda +/- al rededor del objetivo
        float margin; // el maximo permitido obj + margen
    };

    static inline float rad_mm_day(float lat_deg, int doy)
    {
        const float GSC = 0.0820f; // constante solar (MJ m^-2 min^-1)

        float phi = lat_deg * M_PI / 180.0f;                             // latitud en radianes
        float dr = 1.0f + 0.033f * cosf(2.0f * M_PI / 365.0f * doy);     // distancia relativa Tierra-Sol
        float delta = 0.409f * sinf(2.0f * M_PI / 365.0f * doy - 1.39f); // declinación solar

        float arg = -tan(phi) * tan(delta);

        arg = clampf(arg, -1.0f, 1.0f);
        float ws = acosf(arg);
        float ra = (24.0f * 60.0f / M_PI) * GSC * dr *
                   (ws * sinf(phi) * sinf(delta) + cosf(phi) * cosf(delta) * sinf(ws));

        return ra / 2.45f; // MJ m-2 dia-1 > mm/día
    }

    // Valor ET0 de Hargreaves en mm/dia
    static inline float et0_hargreaves(float tmed, float tmax, float tmin, float ra_mm)
    {
        float dt = tmax - tmin;
        if (dt < 0.0f)
            dt = 0.0f;

        float v = 0.0023f * ra_mm * (tmed * 17.8f) * sqrtf(dt);
        return v < 0.0f ? 0.0f : v;
    }

    // Lluvia efectiva a partir del total del dia
    static inline float pef_dia(float p_mm)
    {
        return p_mm < 5.0f ? 0.0f : 0.8f * p_mm;
    }

    static inline float median(float *v, int n)
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

    struct PressureTrend
    {
        float v[13];
        uint8_t n;
        uint8_t last;

        void init()
        {
            n = 0;
            last = 0;
        }

        void sample(float p, uint32_t epoch)
        {
            if (n == 0 || epoch < last || epoch - last >= 900u)
            {
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
        }

        float fall(float p_actual) const
        {
            float mx = p_actual;

            for (int i = 0; i < n; i++)
            {
                if (v[i] > mx)
                    mx = v[i];
            }

            return mx - p_actual;
        }
    };

    // Utilizado para el registro de datos
    // t = temperatura, hr = humedad relativa, p = presion, rad = radiacion/luz
    struct Snapshot
    {
        uint32_t epoch;
        float t, hr, p, rad;
        float rain_day, rain_6h;
        float coffee_soil, bean_soil;
        uint8_t leaf; // Humedad en hojas
        // Profundidad actual del agua, profundidad objetivo y nivel de
        float layer, layer_obj, reservoir;
        // Estado de irrigacion
        uint8_t est_coffee, est_bean, est_rice;
        // Mantienen los niveles de riesgo correspondientes por tipo de grano
        uint8_t risk[3];
        uint8_t h48[3];
        uint8_t stage[3];
        uint8_t season;
        uint8_t blockages, failures, safe, exits;
        float et0, d_coffe, d_bean;
    };

    struct Balance {
        float D;    // agotamiento en mm
        float TAW; // agua disponible total en mm
        float p; // fracción de agotamiento permitida
        float RAW() const { return p * TAW; } 
    };

    // Escribe un numero o el texto 'vacio' si el valor es invalido
    static inline void num_txt(char *buf, size_t n, float v, int dec, const char *vacio)
    {
        if (isnan(v) || isinf(v))
        {
            snprintf(buf, n, "%s", vacio);
        }
        else
        {
            snprintf(buf, n, "%.*f", dec, v);
        }
    }

    static inline void date_text(uint32_t epoch, char *buf, size_t n)
    {
        Time::Date date = Time::from_epoch(epoch);
        snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d", date.year, date.month, date.day, date.hh, date.mi, date.ss);
    }

private:
    static inline float
    clampf(float value, float low, float high)
    {
        return value < low ? low : (value > high ? high : low);
    }
};

#endif