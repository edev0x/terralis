#ifndef FIRMWARE_PINS_H
#define FIRMWARE_PINS_H

#include <stdint.h>

class Pins {
public:
    Pins() = delete;

    // I2C: OLED 0x3C, BMP180 0x77, DS1307 0x68
    static const uint8_t I2C_SDA = 21;
    static const uint8_t I2C_SCL = 22;

    // microSD (VSPI)
    static const uint8_t SD_SCK  = 18;
    static const uint8_t SD_MISO = 19;
    static const uint8_t SD_MOSI = 23;
    static const uint8_t SD_CS   = 5;

    // 74HC595
    static const uint8_t SR_DATA  = 13;
    static const uint8_t SR_CLK   = 14;
    static const uint8_t SR_LATCH = 27;

    // Sensores
    static const uint8_t DHT_DATA     = 4;
    static const uint8_t SOIL_COFFEE = 34;  // ADC1, solo entrada
    static const uint8_t SOIL_BEAN   = 35;  // ADC1, solo entrada
    static const uint8_t LEAF         = 32;  // ADC1
    static const uint8_t LDR          = 33;  // ADC1
    static const uint8_t RAIN         = 25;  // interrupcion, INPUT_PULLUP
    static const uint8_t LAYER_TRIG  = 26;
    static const uint8_t LAYER_ECHO  = 36;  // solo entrada
    static const uint8_t TANK_TRIG  = 16;
    static const uint8_t TANK_ECHO  = 17;

    // Interfaz local
    static const uint8_t BTN_NEXT = 2;
    static const uint8_t BTN_OK  = 12;
    static const uint8_t BUZZER  = 15;

    // Bits del 74HC595
    struct Out {
        Out() = delete;

        static const uint8_t PUMP       = 0;
        static const uint8_t V_COFFEE   = 1;
        static const uint8_t V_BEAN     = 2;
        static const uint8_t V_RICE_IN  = 3;
        static const uint8_t V_RICE_OUT = 4;
        static const uint8_t LED_GREEN  = 5;
        static const uint8_t LED_YELLOW = 6;
        static const uint8_t LED_RED    = 7;
    };
};

#endif  // FIRMWARE_PINS_H