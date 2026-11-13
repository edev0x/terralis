#ifndef FIRMWARE_CONFIG_H
#define FIRMWARE_CONFIG_H

#include <stdint.h>

// Parametros del sistema. Solo constantes: no depende de Arduino ni de FreeRTOS.
namespace Config
{
    constexpr uint8_t VERSION_MAJOR = 1;
    constexpr uint8_t VERSION_MINOR = 0;
    constexpr uint8_t VERSION_PATCH = 0;

    // Factor de aceleracion del tiempo simulado (1 = tiempo real).
    // Con valores altos los periodos reales se acortan; el DHT22 exige >= 2,5 s
    // entre lecturas, por eso no conviene pasar de 20 con el clima cada 60 s.
    constexpr uint32_t TIME_ACCEL = 1;

    // Archivo de registro en la microSD (la biblioteca SD del ESP32 exige la '/' inicial)
    constexpr const char *CSV_FILE = "/registro.csv";

    // Opciones de montaje
    namespace Hardware
    {
        constexpr bool INVERTED_RAD = false;     // true si el modulo LDR da mas voltaje con menos luz
        constexpr bool RELAY_ACTIVE_LOW = false; // true si los modulos de rele se activan con nivel bajo
        constexpr bool FORCE_DATE = false;       // true fuerza 2026-11-08 06:00 al arrancar (epoca seca)
    }

    namespace Network
    {
        constexpr const char *SSID = "WOKWI-GUEST";
        constexpr const char *PASSWORD = "";
        constexpr int32_t CHANNEL = 6;
    }

    namespace MQTT
    {
        constexpr const char *HOST = "broker.hivemq.com";
        constexpr uint16_t PORT = 1883;
        constexpr const char *CLIENT_ID = "terraliesp3201";

        constexpr const char *TELEMETRY_TOPIC = "terralis/sv/telemetry";
        constexpr const char *REGISTRY_TOPIC = "terralis/sv/registry";
        constexpr const char *ALERTS_TOPIC = "terralis/sv/alerts";
    }

    namespace Timing
    {
        // Periodos en tiempo simulado
        constexpr uint32_t WEATHER_MS = 60000UL; // 1 minuto
        constexpr uint32_t WATER_MS = 10000UL;   // 10 segundos
        constexpr uint32_t CONTROL_MS = 10000UL; // 10 segundos
        constexpr uint32_t LOGGER_S = 60UL;      // 60 segundos
        constexpr uint32_t MQTT_MS = 60000UL;    // 1 minuto (tiempo real)
        constexpr uint32_t UI_REFRESH_MS = 200UL;

        constexpr uint32_t atLeast(uint32_t value, uint32_t minimum)
        {
            return value > minimum ? value : minimum;
        }

        // Periodos reales, con el minimo que admite cada sensor
        constexpr uint32_t WEATHER_PERIOD_MS = atLeast(WEATHER_MS / TIME_ACCEL, 2500UL); // DHT22
        constexpr uint32_t WATER_PERIOD_MS = atLeast(WATER_MS / TIME_ACCEL, 1500UL);     // 2 sensores x 5 medidas
        constexpr uint32_t CONTROL_PERIOD_MS = atLeast(CONTROL_MS / TIME_ACCEL, 250UL);

        // Un dato se considera vencido tras 3 periodos + este margen
        constexpr uint32_t STALE_MARGIN_MS = 5000UL;
        constexpr uint32_t staleAfter(uint32_t periodMs) { return 3 * periodMs + STALE_MARGIN_MS; }
    }

    namespace Site
    {
        constexpr float LATITUDE = 13.7f; // El Salvador
        constexpr float LONGITUDE = -88.8f;
    }

    namespace Sensors
    {
        // Distancias de los HC-SR04 (cm)
        constexpr float FLOOR_DISTANCE_CM = 40.0f;
        constexpr float EMPTY_TANK_DISTANCE_CM = 200.0f;
        constexpr float FULL_TANK_DISTANCE_CM = 20.0f;

        constexpr float MM_PER_PULSE = 0.2f;      // lluvia por vuelco del pluviometro
        constexpr uint32_t RAIN_DEBOUNCE_MS = 50; // antirrebote del pluviometro
        constexpr float WET_LEAF_PCT = 50.0f;     // lectura >= este valor: hoja mojada
        constexpr int BAD_READINGS_MAX = 3;       // lecturas malas seguidas que activan la falla
    }

    // Condiciones que suspenden el riego
    namespace Blocking
    {
        constexpr float RAIN_6H_MM = 5.0f;          // lluvia en las ultimas 6 horas
        constexpr float PRESSURE_DROP_HPA = 3.0f;   // caida en 3 horas
        constexpr float RESERVOIR_LOW_PCT = 15.0f;  // bloquea por debajo de este nivel
        constexpr float RESERVOIR_OK_PCT = 20.0f;   // histeresis para liberar el bloqueo
    }

    namespace Climate
    {
        constexpr float ET0_INITIAL = 4.0f; // mm/dia hasta tener el primer dia completo
    }

    namespace Coffee
    {
        constexpr float LOW_HUMIDITY = 35.0f;        // % de humedad que inicia el riego
        constexpr float HIGH_HUMIDITY = 50.0f;       // % de humedad que lo detiene
        constexpr float MAX_IRRIGATION_S = 20.0f * 60.0f;
        constexpr float REST_S = 60.0f * 60.0f;
        constexpr float TAW_MM = 60.0f;              // agua disponible total
        constexpr float DEPLETION_FRACTION = 0.40f;  // fraccion de agotamiento permitida
        constexpr float IRRIGATION_RATE_MM_H = 20.0f;

        constexpr uint8_t N_STAGES = 2;
        constexpr uint8_t DEFAULT_STAGE = 0;
        constexpr float KC[N_STAGES] = {0.90f, 1.05f};
        constexpr const char *const STAGE_NAMES[N_STAGES] = {
            "Sin malezas (0.90)",
            "Con cobertura (1.05)"};
    }

    namespace Bean
    {
        constexpr float LOW_HUMIDITY = 40.0f;
        constexpr float HIGH_HUMIDITY = 55.0f;
        constexpr float MAX_IRRIGATION_S = 15.0f * 60.0f;
        constexpr float REST_S = 45.0f * 60.0f;
        constexpr float TAW_MM = 50.0f;
        constexpr float DEPLETION_FRACTION = 0.45f;
        constexpr float IRRIGATION_RATE_MM_H = 25.0f;

        constexpr uint8_t N_STAGES = 3;
        constexpr uint8_t DEFAULT_STAGE = 1;
        constexpr float KC[N_STAGES] = {0.40f, 1.15f, 0.35f};
        constexpr const char *const STAGE_NAMES[N_STAGES] = {
            "Inicial (0.40)",
            "Medio (1.15)",
            "Final (0.35)"};
    }

    // Arroz: se controla la lamina de agua, no la humedad del suelo
    namespace Rice
    {
        constexpr bool IRRIGATED = true;      // false: arroz de secano, nunca se llena
        constexpr bool LAYER_MODELED = false; // true: la lamina se calcula, no se mide
        constexpr float LAYER_INITIAL_CM = 5.0f;
        constexpr float LAYER_INFLOW_CM_H = 3.0f;         // modelo: aporte de la valvula de entrada
        constexpr float LAYER_DRAIN_CM_H = 4.0f;          // modelo: salida por el drenaje
        constexpr float LAYER_PERCOLATION_MM_DAY = 4.0f;  // modelo: percolacion
        constexpr float LAYER_BAND_CM = 1.0f;             // banda +/- alrededor del objetivo
        constexpr float LAYER_MAX_MARGIN_CM = 3.0f;       // exceso sobre el objetivo que abre el drenaje

        constexpr uint8_t N_STAGES = 4;
        constexpr uint8_t DEFAULT_STAGE = 1;
        constexpr uint8_t DRAIN_STAGE = 3;
        constexpr float KC[N_STAGES] = {1.05f, 1.20f, 1.20f, 0.90f};
        constexpr float LAYER_TARGET_CM[N_STAGES] = {2.5f, 5.0f, 7.0f, 0.0f};
        constexpr const char *const STAGE_NAMES[N_STAGES] = {
            "Establecimiento (2.5cm)",
            "Macollam. (5cm)",
            "Floracion (7cm)",
            "Drenaje final"};
    }

    // Riesgo fitosanitario. Indice: 0 = cafe (roya) | 1 = frijol (mustia/antracnosis) | 2 = arroz (anublo)
    namespace Risk
    {
        constexpr uint8_t SLOTS = 49; // 48 horas + la hora en curso
        constexpr float T_MIN[3] = {18.0f, 18.0f, 20.0f};
        constexpr float T_MAX[3] = {28.0f, 26.0f, 28.0f};
        constexpr float RH_MIN[3] = {0.0f, 90.0f, 90.0f}; // el cafe usa hoja mojada
        constexpr int MEDIUM_HOURS[3] = {12, 10, 12};     // horas favorables en 48 h
        constexpr int HIGH_HOURS[3] = {24, 20, 24};
    }

    // Etapas por cultivo. Indice: 0 = cafe | 1 = frijol | 2 = arroz (Constants::Crop)
    inline uint8_t stage_count(int crop)
    {
        return crop == 0 ? Coffee::N_STAGES : (crop == 1 ? Bean::N_STAGES : Rice::N_STAGES);
    }

    inline uint8_t default_stage(int crop)
    {
        return crop == 0 ? Coffee::DEFAULT_STAGE : (crop == 1 ? Bean::DEFAULT_STAGE : Rice::DEFAULT_STAGE);
    }

    inline const char *stage_name(int crop, uint8_t stage)
    {
        if (stage >= stage_count(crop))
            return "?";
        return crop == 0 ? Coffee::STAGE_NAMES[stage]
                         : (crop == 1 ? Bean::STAGE_NAMES[stage] : Rice::STAGE_NAMES[stage]);
    }
}

#endif // FIRMWARE_CONFIG_H