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

    struct SimulatorTime {
        SimulatorTime() = delete;

        static const uint16_t WEATHER_MS = 60_000UL;  // 1 minuto
        static const uint16_t WATER_MS  = 10_000UL;  // 10 segundos
        static const uint16_t CONTROL_MS = 10_000UL;  // 10 segundos
        static const uint16_t LOGGER_SECONDS = 60UL;  // 60 segundos
        static const uint16_t MQTT_MS = 60_000UL;  // 1 minuto
        static const uint16_t UI_REFRESH_MS = 200UL;  // 200 milisegundos
    };


};

#endif 