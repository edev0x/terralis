#ifndef FIRMWARE_CONFIG_H
#define FIRMWARE_CONFIG_H

#include <stdint.h>

class Config {
public:
    Config() = delete;  // solo constantes, no se instancia

    static const uint8_t VERSION_MAJOR = 1;
    static const uint8_t VERSION_MINOR = 0;
    static const uint8_t VERSION_PATCH = 0;


    // Factor de aceleración del tiempo simulado (1 = tiempo real)
    // Con valores altos los periodos reales se acortan;
    // el DHT22 exige >= 2.5s entre lecturas, por eso el ACEL no debe pasar de 20 con tClima de 60s.
    static const uint8_t DEFAULT_ACEL = 1;

    // Valor por defecto del archivo CSV de registro de datos en MicroSD
    static const std::string CSV_DEFAULT_NAME = "registro.csv";

    struct Network {
        Network() = delete;

        static const std::string SSID = "WOKWI-GUEST";
        static const std::string PASSWORD = "";
    };
    

    struct MQTT {
        MQTT() = delete;

        static const std::string HOST  = "broker.hivemq.com";
        static const uint16_t PORT = 1883;
        static const std::string CLIENT_ID = "terraliesp3201";

        static const std::string TELEMETRY_TOPIC = "terralis/sv/telemetry";
        static const std::string REGISTRY_TOPIC = "terralis/sv/registry";
        static const std::string ALERTS_TOPIC = "terralis/sv/alerts";
    };

    struct Time {
        Time() = delete;

        static const uint16_t T_CLIMA = 60_000UL;  // 1 minuto
        static const uint16_t T_AGUA  = 10_000UL;  // 10 segundos
        static const uint16_t CONTROL_MS = 10_000UL;  // 10 segundos
        static const uint16_t LOGGER_SECONDS = 60UL;  // 60 segundos
        static const uint16_t MQTT_MS = 60_000UL;  // 1 minuto
        static const uint16_t UI_REFRESH_MS = 200UL;  // 200 milisegundos
    };

    struct GeoInfo {
        GeoInfo() = delete;

        static const float LATITUDE  = 13.7f; // El Salvador
        static const float LONGITUDE = -88.8f;
    };


    struct ClimateModel {
        ClimateModel() = delete;

        // Valor inicial de referencia para ET0
        static const float ET0_INITIAL = 4.0f; 

        // Porcentaje de humedad del suelo en la capa superficial; Si la lectura >= WET_LEAF_PCT = la hoja está humeda
        static const float WET_LEAF_PCT = 50.0f;
        static const float MODELED_SURFACE_LAYER_DEPTH = 0.0f;
        static const float INITIAL_SURFACE_LAYER_DEPTH = 5.0f; // Profundidad inicial de la capa superficial del suelo (cm)
        static const float INFLOW_SURFACE_LAYER_DEPTH = 3.0f; // Profundidad de la capa superficial considerada para la entrada de agua (cm)
        static const float DRAINAGE_SURFACE_LAYER_DEPTH = 4.0f; // Profundidad de la capa superficial considerada para el drenaje (cm)
        static const float SURFACE_LAYER_PERCOLATION_MM_PER_DAY = 4.0f; // Tasa de percolación de agua de la capa superficial (mm/día)
    };

    struct Sensors {
        Sensors() = delete;

        // Valores de distancia en cm.
        static const float SOIL_DISTANCE = 40.0f;
        static const float EMPTY_TANK_DISTANCE = 200.0f;
        static const float FULL_TANK_DISTANCE = 20.0f;

        static const float MM_PER_TICK = 0.2f; // Milímetros de agua por pulso del sensor de flujo
        static const float RAIN_BLOCK_THRESHOLD = 5.0f; // Umbral para ultimas 6 horas
        static const float PRESSURE_HPA = 3.0f; // en 3 horas

        static const float LOW_RESERVOIR_THRESHOLD = 15.0f; // Porcentaje de agua en el tanque para considerar bajo nivel
        static const float OK_RESERVOIR_THRESHOLD = 20.0f; // Histeresis para liberar el bloqueo 

        // Valores de riego de café y frijol
        static const float COFFEE_LOW_HUMIDITY = 35.0f;
        static const float COFFEE_HIGH_HUMIDITY = 50.0f;
        static const float COFFEE_MAX_TIME = (20.0f * 60.0f); // 20 minutos
        static const float IDLE_COFFEE_TIME = (60.0f * 60.0f); // 1 hora

        static const float BEAN_LOW_HUMIDITY = 40.0f;
        static const float BEAN_HIGH_HUMIDITY = 55.0f;
        static const float BEAN_MAX_TIME = (15.0f * 60.0f); // 15 minutos
        static const float IDLE_BEAN_TIME = (45.0f * 60.0f); // 45 minutos
        
        // Agua disponible para riego (mm) y fracción de agotamiento permitida.
        static const float TAW_COFFEE = 60.0f;
        static const float TAW_BEAN = 50.0f;

        static const float COFFEE_ALLOWED_DEPLETION = 0.40f;
        static const float BEAN_ALLOWED_DEPLETION = 0.45f;

        static const float COFFEE_IRRIGATION_THRESHOLD = 20.0f;
        static const float BEAN_IRRIGATION_THRESHOLD = 25.0f;

        // Etapas fenologicas de los cultivos
        static const uint8_t N_COFFEE_STAGES = 2;
        static const uint8_t N_BEAN_STAGES = 3;
        static const uint8_t N_RICE_STAGES = 4;
        static const uint8_t RICE_DRAIN_STAGE = 3;

        static const float KC_COFFEE[N_COFFEE_STAGES] = { 0.90f, 1.05f }; // Coeficientes de cultivo para café
        static const float KC_BEAN[N_BEAN_STAGES] = { 0.40f, 1.15f, 0.35f }; // Coeficientes de cultivo para frijol
        static const float KC_RICE[N_RICE_STAGES] = { 1.05f, 1.20f, 1.20f, 0.90f }; // Coeficientes de cultivo para arroz

        static const float LAYER_OBJ_CM[N_RICE_STAGES] = { 2.5f, 5.0f, 7.0f, 0.0f }; // Profundidad de la capa superficial del suelo para cada etapa del arroz (cm)

        static const float LAYER_BAND = 1.0f; // Ancho de la banda de profundidad para el cálculo de la capa superficial del suelo (cm)
        static const float LAYER_MAX_MARGIN = 3.0f; // Margen máximo de profundidad para la capa superficial del suelo (cm)
        static const uint8_t IRRIGATING_RICE = 1; 

        static const char *const COFFEE_STAGES_NAMES[N_COFFEE_STAGES] = {
            "Sin malezas (0.90)",
            "Con cobertura (1.05)"
        };

        static const char *const BEAN_STAGES_NAMES[N_BEAN_STAGES] = {
            "Inicial (0.40)",
            "Medio (1.15)",
            "Final (0.35)"
        };

        static const char *const RICE_STAGES_NAMES[N_RICE_STAGES] = {
            "Establecimiento (2.5cm)",
            "Macollam. (5cm)",
            "Floración (7cm)",
            "Drenaje final"
        };

        // Riesgo fitosanitario
        // Indice = 0 cafe (roya) | 1 = frijol (mustia/antracnosis) | 2 = arroz (anublo)
        static const float RISK_MIN[3] = {18.0f, 18.0f, 20.0f};
        static const float RISK_MAX[3] = {28.0f, 26.0f, 28.0f};
        static const float RISK_HR_MIN[3] = { 0.0f, 90.0f, 90.0f }; // El cafe usa hoja humeda

        static const int MEDIUM_RISK[3] = { 12, 10, 12 };
        static const int HIGH_RISK[3] = { 24, 20, 24 };
    }
};

#endif 