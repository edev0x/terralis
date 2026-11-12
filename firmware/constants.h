#ifndef FIRMWARE_CONSTANTS_H
#define FIRMWARE_CONSTANTS_H

#include <stdint.h>

class Constants
{
public:
    Constants() = delete;

    enum CodeAlert : uint8_t
    {
        AL_COFFEE_RISK = 0,
        AL_BEAN_RISK,
        AL_RICE_RISK,
        AL_RESERVOIR,
        AL_FAILURE,
        AL_PRESSURE,
        AL_N
    };

    enum LayerState : uint8_t
    {
        LAYER_IN_BAND = 0,
        LAYER_FILLING = 1,
        LAYER_DRAINING = 2
    };

    enum Failure : uint8_t
    {
        FL_WEATHER = 1,
        FL_LAYER = 2,
        FL_TANK = 3
    };

    enum Blockage : uint8_t
    {
        BL_RAIN = 1,
        BL_PRESSURE = 2,
        BL_RESERVOIR = 3,
        BL_FAIULRE = 4
    };

    const enum Season : uint8_t
    {
        LLUVIOSA = 0,
        CANICULA = 1,
        SECA = 2
    };

    static uint8_t get_season(uint8_t month, uint8_t day)
    {
        if ((month == 7 && day >= 15) || (month == 8 && day <= 15))
        {
            return CANICULA;
        }
        else if (month >= 5 && month <= 10)
        {
            return LLUVIOSA;
        }
        else
        {
            return SECA;
        }
    }

    inline static const char *alert_code(uint8_t code)
    {
        static const char *const n[AL_N] = {
            "RIESGO_CAFE", "RIESGO_FRIJOL", "RIESGO_ARROZ",
            "RESERVORIO_BAJO", "FALLA_SENSOR", "PRESION_CAYENDO"};

        return code < AL_N ? n[code] : "?";
    }

    inline static const char *alert_message(uint8_t code)
    {
        static const char *const m[AL_N] = {
            "Riesgo alto de roya: inspeccionar el cafetal",
            "Riesgo alto de mustia o antracnosis: inspeccionar el frijol",
            "Riesgo alto de anublo: inspeccionar el arrozal",
            "Reservorio bajo: riego bloqueado",
            "Sensor fuera de rango: modo seguro, valvulas cerradas",
            "Caida rapida de presion: tormenta proxima, riego suspendido"};

        return code < AL_N ? m[code] : "?";
    }

    inline static const char *irrigation_state(uint8_t stage)
    {
        static const char *const n[3] = {
            "EN_RANGO", "REGANDO", "REPOSO"};

        return stage < 3 ? n[stage] : "?";
    }

    inline static const char *layer_name(uint8_t state)
    {
        static const char *const n[3] = {
            "EN_BANDA", "LLENANDO", "DRENANDO"};

        return state < 3 ? n[state] : "?";
    }

    inline static const char *risk_detail(uint8_t n)
    {
        static const char *const t[3] = { "BAJO", "MEDIO", "ALTO" };
        return n < 3 ? t[n] : "?";
    }

    inline static const char *time_of_year(uint8_t e) {
        static const char *const n[3] = { "LLUVIOSA", "CANICULA", "SECA"};
        return e < 3 ? n[e] : "?";
    }

    static const uint8_t INVERTED_RAD  = 0; // 1 si el modulo LDR da mas voltaje con menos luz
    static const uint8_t ACTIVE_RELAY_LOW = 0; // 1 si los modulos del rele se activan con nivel bajo
    static const uint8_t FORCE_DATE = 0; // 1 - Se fuerza fecha 2026-11-08 06:00 para epoca seca
};

#endif