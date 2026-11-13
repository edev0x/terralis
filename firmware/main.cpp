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

    if (us == 0)
        return NAN;
    return us / 58.0f;
}

// Mediana de 5 medidas; NaN si hay menos de 3 válidas
// El rango de HC-SR05 es de 2 a 400 cm
static float medianDistance(int trig, int echo)
{
    float v[5];
    int n = 0;
    for (int i = 0; i < 5; i++)
    {
        float d = distanceCm(trig, echo);
        if (!isnan(d) && d >= 2.0f && d <= 400.0f)
            v[n++] = d;

        vTaskDelay(pdMS_TO_TICKS(60));
    }

    if (n < 3)
        return NAN;

    return Utils::median(v, n);
}

static void buzzer(bool on)
{
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWriteTone(Pins::BUZZER, on ? 2000 : 0);
#else
    ledcWriteTone(0, on ? 2000 : 0);
#endif
}

static void saveStage(int i)
{
    if (xSemaphoreTake(mtxNVS, pdMS_TO_TICKS(50)))
    {
        char k[3] = {'e', (char)('0' + i), 0};
        prefs.putChar(k, stage[i]);
        xSemaphoreGive(mtxNVS);
    }
}

static void blockageText(uint8_t b, char *out, size_t n)
{
    out[0] = 0;
    if (b & Constants::BL_RAIN)
        strlcat(out, "LL ", n);
    if (b & Constants::BL_PRESSURE)
        strlcat(out, "PR ", n);
    if (b & Constants::BL_RESERVOIR)
        strlcat(out, "RE ", n);
    if (b & Constants::BL_FAIULRE)
        strlcat(out, "FA ", n);

    if (!out[0])
        strlcpy(out, "ninguno", n);
}

// Pluviometro
void IRAM_ATTR isrRain()
{
    uint32_t t = millis();
    BaseType_t wake = pdFALSE;

    xQueueSendFromISR(q_pulse, &t, &wake);
    if (wake)
        portYIELD_FROM_ISR();
}

// Procesa los pulsos del pluviometro: descarta rebotes separados por menos de
// 50 ms y envia a q_rain la lluvia correspondiente a cada pulso valido.
void tRain(void *)
{
    uint32_t t, last = 0;
    bool first = true;

    for (;;)
    {
        hearth_beats[Config::HearthBeats::H_RAIN] = millis();

        if (xQueueReceive(q_pulse, &t, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            if (first || (uint32_t)(t - last) >= 50UL)
            {
                first = false;
                last = t;

                float mm = Config::Sensors::MM_PER_PULSE;
                xQueueSend(q_rain, &mm, 0);
            }
        }
    }
}


// Mide periódicamente la humedad del suelo y los niveles de la capa superficial y del tanque,
// y publica las lecturas en la cola q_water.
void tWater(void *) 
{
    TickType_t lastTick = xTaskGetTickCount();

    for (;;) {
        hearth_beats[Config::HearthBeats::H_WATER] = millis();

        Config::WaterMeasure water_measure;
        water_measure.soil_c = adcPercentage(Pins::SOIL_COFFEE);
        water_measure.soil_f = adcPercentage(Pins::SOIL_BEAN);

        water_measure.layer_ok = true;
        water_measure.layer = NAN;

        if (Config::ClimateModel::MODELED_SURFACE_LAYER_DEPTH == 1.0f) {
            float dl = medianDistance(Pins::LAYER_TRIG, Pins::LAYER_ECHO);
            if (isnan(dl)) {
                water_measure.layer_ok = false;
            } else {
                water_measure.layer = Config::Sensors::FLOOR_DISTANCE - dl;

                if (water_measure.layer < 0.0f) water_measure.layer = 0.0f;
            }
        }

        float dt = medianDistance(Pins::TANK_TRIG, Pins::TANK_ECHO);
        if (isnan(dt))
        {
            water_measure.tank_ok = false;
            water_measure.reserv = NAN;
        }
        else
        {
            water_measure.tank_ok = true;
            water_measure.reserv = Utils::clampf((Config::Sensors::EMPTY_TANK_DISTANCE - dt) * 100.0f / (Config::Sensors::EMPTY_TANK_DISTANCE - Config::Sensors::FULL_TANK_DISTANCE),
                              0.0f, 100.0f);
        }
        xQueueOverwrite(q_water, &water_measure);
        vTaskDelayUntil(&lastTick, pdMS_TO_TICKS(PER_WATER_MILLS));
    }
}

void tControl(void *)
{
    Utils::RainTracker rain;
    rain.init();

    Utils::PressureTrend trend;
    trend.init();


    Utils::FungalRisk risk[3];

}


void setup()
{
    // Setup code here, to run once:
}

void loop()
{
    // Main code here, to run repeatedly:
}