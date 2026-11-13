#ifndef FIRMWARE_CONSTANTS_H
#define FIRMWARE_CONSTANTS_H

#include <stdint.h>
#include <stdio.h>

// Codigos, estados y sus nombres. No depende de Arduino.
namespace Constants
{
    enum Crop : uint8_t
    {
        CROP_COFFEE = 0,
        CROP_BEAN,
        CROP_RICE,
        N_CROPS
    };

    enum CodeAlert : uint8_t
    {
        AL_COFFEE_RISK = 0, // AL_COFFEE_RISK + Crop da la alerta de cada cultivo
        AL_BEAN_RISK,
        AL_RICE_RISK,
        AL_RESERVOIR,
        AL_FAILURE,
        AL_PRESSURE,
        AL_N
    };

    enum IrrigationState : uint8_t
    {
        IR_IN_RANGE = 0,
        IR_IRRIGATING = 1,
        IR_IDLE = 2
    };

    enum LayerState : uint8_t
    {
        LAYER_IN_BAND = 0,
        LAYER_FILLING = 1,
        LAYER_DRAINING = 2
    };

    // Mascaras de bits: se combinan con |
    enum Failure : uint8_t
    {
        FL_WEATHER = 1 << 0,
        FL_LAYER = 1 << 1,
        FL_TANK = 1 << 2
    };

    enum Blockage : uint8_t
    {
        BL_RAIN = 1 << 0,
        BL_PRESSURE = 1 << 1,
        BL_RESERVOIR = 1 << 2,
        BL_FAILURE = 1 << 3
    };

    enum Season : uint8_t
    {
        RAINY = 0,
        DRY_SPELL = 1, // canicula
        DRY = 2
    };

    // Latidos de las tareas para el vigilante por software
    enum Heartbeat : uint8_t
    {
        H_RAIN = 0,
        H_WATER,
        H_CONTROL,
        H_WEATHER,
        H_LOGGER,
        H_MQTT,
        H_UI,
        H_N
    };

    inline uint8_t get_season(int month, int day)
    {
        if ((month == 7 && day >= 15) || (month == 8 && day <= 15))
            return DRY_SPELL;
        if (month >= 5 && month <= 10)
            return RAINY;
        return DRY;
    }

    inline const char *alert_code(uint8_t code)
    {
        static const char *const n[AL_N] = {
            "RIESGO_CAFE", "RIESGO_FRIJOL", "RIESGO_ARROZ",
            "RESERVORIO_BAJO", "FALLA_SENSOR", "PRESION_CAYENDO"};
        return code < AL_N ? n[code] : "?";
    }

    inline const char *alert_message(uint8_t code)
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

    inline const char *irrigation_state(uint8_t state)
    {
        static const char *const n[3] = {"EN_RANGO", "REGANDO", "REPOSO"};
        return state < 3 ? n[state] : "?";
    }

    inline const char *layer_name(uint8_t state)
    {
        static const char *const n[3] = {"EN_BANDA", "LLENANDO", "DRENANDO"};
        return state < 3 ? n[state] : "?";
    }

    inline const char *risk_detail(uint8_t level)
    {
        static const char *const n[3] = {"BAJO", "MEDIO", "ALTO"};
        return level < 3 ? n[level] : "?";
    }

    inline const char *season_name(uint8_t season)
    {
        static const char *const n[3] = {"LLUVIOSA", "CANICULA", "SECA"};
        return season < 3 ? n[season] : "?";
    }

    // Texto corto con los bloqueos activos, por ejemplo "LL RE "
    inline void blockage_text(uint8_t b, char *out, size_t n)
    {
        snprintf(out, n, "%s%s%s%s",
                 (b & BL_RAIN) ? "LL " : "", (b & BL_PRESSURE) ? "PR " : "",
                 (b & BL_RESERVOIR) ? "RE " : "", (b & BL_FAILURE) ? "FA " : "");
        if (!out[0])
            snprintf(out, n, "ninguno");
    }
}

#endif