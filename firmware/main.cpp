#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <Preferences.h>

// Librerias de terceros
// #include <PubSubClient.h>
// #include <DHT.h>
// #include <Adafruit_BMP085.h>
// #include <Adafruit_GFX.h>
// #include <Adafruit_SSD1306.h>
// #include <RTClib.h>

#include "config.h"
#include "constants.h"
#include "logger.h"
#include "pins.h"
#include "utils.h"

// Periodos reales en milisegundos
#define MAXU(a, b) ((a) > (b) ? (a) : (b))
#define PER_WEATHER_MILLS MAXU(Config::Time::T_CLIMA / Config::DEFAULT_ACEL, 2500UL)
#define PER_WATER_MILLS MAXU(Config::Time::T_AGUA / Config::DEFAULT_ACEL, 1500UL) // 2 sensores
#define PER_CONTROL_MILLS MAXU(Config::Time::CONTROL_MS / Config::DEFAULT_ACEL, 250UL)

// Objetos globales
DHT dht(Pins::DHT_DATA, DHT22);
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
RTC_DS1307 rtc;
WiFiClient wifiClient;
PubSubClient mqtt(wifiCient);
Preferences prefs;

SemaphoreHandle_t mtxI2C, mtxSD, mtxNVS;
QueueHandle_t q_pulse, q_rain, q_water, q_log, q_ui, q_mqtt, q_alert;

// Estados compartidos
volatile uint8_t stage[3] = {0, 1, 1};
volatile bool snooze = false;
volatile bool enableAlarm = false;
volatile bool hangControl = false; // Comando 'x' en consola serial
volatile bool noNetwork = false;   // Comando 'w' para simular que no hay red disponible
volatile bool sdOk = false;
volatile bool netOk = false;
volatile bool bmpOk = false;

volatile uint32_t hearth_beats[Config::HearthBeats::H_N];

static const uint32_t MAX_HEARTH_BEAT[Config::HearthBeats::H_N]{
    5000UL,                         // tLluvia - 5 segundos
    3 * PER_WATER_MILLS + 5000UL,   // tAgua - 3 periodos de agua + 5 segundos
    2 * PER_CONTROL_MILLS + 5000UL, // tControl - 2 periodos de control + 5 segundos
    3 * PER_WEATHER_MILLS + 5000UL, // tClima - 3 periodos de clima + 5 segundos
    10000UL,                        // tLogger - 10 segundos
    30000UL,                        // tMqtt - 30 segundos
    50000UL};                       // tUI - 50 segundos

static const char *const HB_NAME[Config::HearthBeats::H_N] = {
    "tLluvia", "tAgua", "tControl", "tClima",
    "tLogger", "tMQTT", "tUI"};

// Tiempo simulado
static uint32_t epoch0 = 0;
static uint32_t mills0 = 0;

uint32_t simEpoch()
{
    return epoch0 + (uint32_t)(((uint64_t)(millis() - mills0) * Config::DEFAULT_ACEL) / 1000ULL);
}

void write595(uint8_t logical)
{
    uint8_t physical = logical;

    if (Constants::ACTIVE_RELAY_LOW)
    {
        physical ^= 0x1F; // bits 0-4 son relays y leds (5-7) siempre son activo altos
    }

    digitalWrite(PIN_595_LATCH, LOW);
    shiftOut(PIN_595_DATA, PIN_595_CLK, MSBFIRST, physical);
    digitalWrite(PIN_595_LATCH, HIGH);
}

static float adcPercentage(int pin)
{
    return analogRead(pin) * 100.0f / 4095.0f; 
}

static float distanceCm(int trig, int echo)
{
    digitalWrite(trig, LOW);
    delayMicroseconds(2);
    digitalWrite(trig, HIGH);
    delayMicroseconds(10);
    digitalWrite(trig, LOW);
    
    unsigned long us = pulseIn(echo, HIGH, 30000UL);

    if (us == 0) return NAN;
    return us / 58.0f;
}

void setup()
{
    // Setup code here, to run once:
}

void loop()
{
    // Main code here, to run repeatedly:
}