#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <Preferences.h>
#include <atomic>

// Bibliotecas de terceros
#include <PubSubClient.h>
#include <DHT.h>
#include <Adafruit_BMP085.h> // el BMP180 usa esta biblioteca
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RTClib.h>

#include "config.h"
#include "constants.h"
#include "control.h"
#include "display.h"
#include "logger.h"
#include "pins.h"
#include "utils.h"

namespace Timing = Config::Timing;
using Utils::Snapshot;

struct MqttSession; // definidos mas abajo, junto al codigo que los usa
struct ControlStats;

// Objetos globales
static DHT dht(Pins::DHT_DATA, DHT22);
static Adafruit_BMP085 bmp;
static Adafruit_SSD1306 oled(128, 64, &Wire, -1);
static RTC_DS1307 rtc;
static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);
static Preferences prefs;

static SemaphoreHandle_t mtxI2C, mtxSD, mtxNVS;
static QueueHandle_t q_pulse, q_rain, q_weather, q_water, q_log, q_ui, q_mqtt, q_alert;

// Estado compartido entre tareas
static std::atomic<uint8_t> stage[Constants::N_CROPS]; // etapa de cafe, frijol y arroz
static std::atomic<bool> snooze{false};                // alarma silenciada por el usuario
static std::atomic<bool> alarmActive{false};
static std::atomic<bool> hangControl{false}; // comando 'x'
static std::atomic<bool> noNetwork{false};   // comando 'w'
static std::atomic<bool> sdOk{false}, netOk{false}, bmpOk{false};
static std::atomic<uint32_t> heartbeats[Constants::H_N];

static inline void beat(uint8_t task) { heartbeats[task] = millis(); }

// Toma un mutex con tiempo limite y lo suelta al salir del bloque
class Lock
{
public:
  Lock(SemaphoreHandle_t m, uint32_t timeoutMs)
      : mutex(m), held(xSemaphoreTake(m, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {}
  ~Lock()
  {
    if (held)
      xSemaphoreGive(mutex);
  }
  explicit operator bool() const { return held; }
  Lock(const Lock &) = delete;
  Lock &operator=(const Lock &) = delete;

private:
  SemaphoreHandle_t mutex;
  bool held;
};

// Tiempo simulado
static uint32_t epoch0 = 0;
static uint32_t millis0 = 0;

static uint32_t simEpoch()
{
  return epoch0 + (uint32_t)(((uint64_t)(millis() - millis0) * Config::TIME_ACCEL) / 1000ULL);
}

// Utilidades de hardware
static void write595(uint8_t logical)
{
  uint8_t physical = logical;
  if (Config::Hardware::RELAY_ACTIVE_LOW)
    physical ^= Pins::Out::RELAY_MASK;

  digitalWrite(Pins::SR_LATCH, LOW);
  shiftOut(Pins::SR_DATA, Pins::SR_CLK, MSBFIRST, physical);
  digitalWrite(Pins::SR_LATCH, HIGH);
}

static float adcPercentage(int pin) { return analogRead(pin) * 100.0f / 4095.0f; }

static float distanceCm(int trig, int echo)
{
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);

  unsigned long us = pulseIn(echo, HIGH, 30000UL);
  return us == 0 ? NAN : us / 58.0f;
}

// Mediana de 5 medidas; NAN si hay menos de 3 validas (rango del HC-SR04: 2 a 400 cm)
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
  return n < 3 ? NAN : Utils::median(v, n);
}

static void buzzer(bool on)
{
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(Pins::BUZZER, on ? 2000 : 0);
#else
  ledcWriteTone(0, on ? 2000 : 0);
#endif
}

// Etapas de los cultivos
// Se guardan en memoria no volatil con las claves "e0", "e1" y "e2"
static void saveStage(int crop)
{
  Lock lock(mtxNVS, 500);
  if (!lock)
    return;
  char key[3] = {'e', (char)('0' + crop), 0};
  prefs.putUChar(key, stage[crop]);
}

static void loadStages()
{
  prefs.begin("finca", false);
  for (int i = 0; i < Constants::N_CROPS; i++)
  {
    char key[3] = {'e', (char)('0' + i), 0};
    uint8_t st = prefs.getUChar(key, Config::default_stage(i));
    stage[i] = st < Config::stage_count(i) ? st : 0;
  }
}

static void nextStage(int crop)
{
  stage[crop] = (stage[crop] + 1) % Config::stage_count(crop);
  saveStage(crop);
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

// tRain: descarta rebotes y convierte cada vuelco del pluviometro en milimetros
void tRain(void *)
{
  uint32_t t, last = 0;
  bool first = true;

  for (;;)
  {
    beat(Constants::H_RAIN);
    if (xQueueReceive(q_pulse, &t, pdMS_TO_TICKS(1000)) != pdTRUE)
      continue;
    if (first || (uint32_t)(t - last) >= Config::Sensors::RAIN_DEBOUNCE_MS)
    {
      first = false;
      last = t;
      float mm = Config::Sensors::MM_PER_PULSE;
      xQueueSend(q_rain, &mm, 0);
    }
  }
}

// Clima
static bool weatherInRange(const Utils::WeatherMeasure &m)
{
  return !isnan(m.temp) && m.temp > -10.0f && m.temp < 60.0f &&
         !isnan(m.humidity) && m.humidity >= 0.0f && m.humidity <= 100.0f &&
         !isnan(m.pressure) && m.pressure > 300.0f && m.pressure < 1100.0f;
}

// tWeather: DHT22, BMP180, humedad de hoja y luz
void tWeather(void *)
{
  TickType_t lastWake = xTaskGetTickCount();

  for (;;)
  {
    beat(Constants::H_WEATHER);

    Utils::WeatherMeasure m;
    m.temp = dht.readTemperature();
    m.humidity = dht.readHumidity();
    m.pressure = NAN;
    if (bmpOk)
    {
      Lock lock(mtxI2C, 500);
      if (lock)
        m.pressure = bmp.readPressure() / 100.0f;
    }
    m.leaf = adcPercentage(Pins::LEAF);
    float light = adcPercentage(Pins::LDR);
    m.rad = Config::Hardware::INVERTED_RAD ? 100.0f - light : light;
    m.ok = weatherInRange(m);
    xQueueOverwrite(q_weather, &m);

    // Con el tiempo acelerado, mantiene el RTC alineado con el tiempo simulado
    if (Config::TIME_ACCEL != 1)
    {
      Lock lock(mtxI2C, 500);
      if (lock)
        rtc.adjust(DateTime(simEpoch()));
    }
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(Timing::WEATHER_PERIOD_MS));
  }
}

// Agua
// tWater: humedad de suelo, lamina del arroz y nivel del reservorio
void tWater(void *)
{
  TickType_t lastWake = xTaskGetTickCount();

  for (;;)
  {
    beat(Constants::H_WATER);

    Utils::WaterMeasure m;
    m.soil_coffee = adcPercentage(Pins::SOIL_COFFEE);
    m.soil_bean = adcPercentage(Pins::SOIL_BEAN);

    m.layer_ok = true;
    m.layer = NAN;
    if (!Config::Rice::LAYER_MODELED)
    {
      float d = medianDistance(Pins::LAYER_TRIG, Pins::LAYER_ECHO);
      m.layer_ok = !isnan(d);
      if (m.layer_ok)
        m.layer = max(Config::Sensors::FLOOR_DISTANCE_CM - d, 0.0f);
    }

    const float EMPTY = Config::Sensors::EMPTY_TANK_DISTANCE_CM, FULL = Config::Sensors::FULL_TANK_DISTANCE_CM;
    float d = medianDistance(Pins::TANK_TRIG, Pins::TANK_ECHO);
    m.tank_ok = !isnan(d);
    m.reservoir = m.tank_ok ? Utils::clampf((EMPTY - d) * 100.0f / (EMPTY - FULL), 0.0f, 100.0f) : NAN;

    xQueueOverwrite(q_water, &m);
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(Timing::WATER_PERIOD_MS));
  }
}

// Control
// La logica esta en control.h; aqui solo se conecta con colas, salidas y puerto serie.
struct ControlStats
{
  uint32_t droppedAlerts, droppedLogs, lateCycles;
};

static void readControlInputs(Control::Input &in)
{
  in.ms = millis();
  in.epoch = simEpoch();

  in.rain_mm = 0.0f;
  float mm;
  while (xQueueReceive(q_rain, &mm, 0) == pdTRUE)
    in.rain_mm += mm;

  in.has_weather = xQueueReceive(q_weather, &in.weather, 0) == pdTRUE;
  in.has_water = xQueueReceive(q_water, &in.water, 0) == pdTRUE;
  for (int i = 0; i < Constants::N_CROPS; i++)
    in.stage[i] = stage[i];
}

static void emitAlerts(uint8_t alerts, uint32_t epoch, const char *stamp, ControlStats &stats)
{
  for (uint8_t code = 0; code < Constants::AL_N; code++)
  {
    if (!(alerts & (1u << code)))
      continue;
    Utils::Alert alert = {code, epoch};
    if (xQueueSend(q_alert, &alert, 0) != pdTRUE)
      stats.droppedAlerts++;
    snooze = false; // una alerta nueva vuelve a activar el zumbador
    Serial.printf("[%s] ALERTA %s: %s\n", stamp, Constants::alert_code(code), Constants::alert_message(code));
  }
}

// Cambios de estado desde el ciclo anterior
static void printChanges(const Snapshot &prev, const Snapshot &s, bool first, const char *stamp)
{
  if (s.state_coffee != prev.state_coffee)
    Serial.printf("[%s] CAFE   %s -> %s\n", stamp, Constants::irrigation_state(prev.state_coffee),
                  Constants::irrigation_state(s.state_coffee));
  if (s.state_bean != prev.state_bean)
    Serial.printf("[%s] FRIJOL %s -> %s\n", stamp, Constants::irrigation_state(prev.state_bean),
                  Constants::irrigation_state(s.state_bean));
  if (s.state_rice != prev.state_rice)
    Serial.printf("[%s] ARROZ  %s -> %s (lamina %.1f cm, objetivo %.1f)\n", stamp,
                  Constants::layer_name(prev.state_rice), Constants::layer_name(s.state_rice), s.layer,
                  s.layer_target);
  if (first || s.blockages != prev.blockages)
  {
    char txt[24];
    Constants::blockage_text(s.blockages, txt, sizeof(txt));
    Serial.printf("[%s] BLOQUEOS: %s\n", stamp, txt);
  }
}

// Fila CSV por el puerto serie y, si hubo problemas, los contadores de diagnostico
static void printSummary(const Snapshot &s, const ControlStats &stats, const char *stamp)
{
  char row[256];
  Logger::format_csv(s, row, sizeof(row));
  Serial.println(row);
  if (stats.droppedAlerts || stats.droppedLogs || stats.lateCycles)
    Serial.printf("[%s] DIAG alertas perdidas=%lu, registros perdidos=%lu, ciclos tardios=%lu\n", stamp,
                  (unsigned long)stats.droppedAlerts, (unsigned long)stats.droppedLogs,
                  (unsigned long)stats.lateCycles);
}

void tControl(void *)
{
  static Control::Controller controller; // fuera de la pila de la tarea
  controller.init(millis());

  Control::Input in = Control::Input();
  Control::Output out;
  Snapshot prev;
  memset(&prev, 0, sizeof(prev));
  ControlStats stats = {0, 0, 0};
  bool first = true;
  uint32_t lastLog = 0, lastSummary = 0; // epoch simulado
  uint32_t lastMqttMs = 0;               // tiempo real
  TickType_t lastWake = xTaskGetTickCount();

  for (;;)
  {
    if (hangControl) // prueba E12: la tarea deja de dar latidos
    {
      for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    beat(Constants::H_CONTROL);

    readControlInputs(in);
    controller.step(in, out);
    write595(out.outputs); // sin datos (out.ready == false) todo queda cerrado

    if (out.ready)
    {
      const Snapshot &s = out.snap;
      char stamp[24];
      Time::date_text(in.epoch, stamp, sizeof(stamp));

      emitAlerts(out.alerts, in.epoch, stamp, stats);
      alarmActive = out.alarm && !snooze;

      xQueueOverwrite(q_ui, &s);
      if (first || in.epoch - lastLog >= Timing::LOGGER_S)
      {
        lastLog = in.epoch;
        if (xQueueSend(q_log, &s, 0) != pdTRUE)
          stats.droppedLogs++;
      }
      if (first || in.ms - lastMqttMs >= Timing::MQTT_MS)
      {
        lastMqttMs = in.ms;
        xQueueOverwrite(q_mqtt, &s);
      }

      printChanges(prev, s, first, stamp);
      if (first || in.epoch - lastSummary >= 60) // resumen cada minuto simulado
      {
        lastSummary = in.epoch;
        printSummary(s, stats, stamp);
      }
      prev = s;
      first = false;
    }

    if (millis() - in.ms > Timing::CONTROL_PERIOD_MS)
      stats.lateCycles++;
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(Timing::CONTROL_PERIOD_MS));
  }
}

// Registro en la SD
static bool saveLine(const char *line)
{
  static uint32_t lastRetry = 0;

  Lock lock(mtxSD, 2000);
  if (!lock)
    return false;
  if (!sdOk && millis() - lastRetry > 60000UL)
  {
    lastRetry = millis();
    sdOk = SD.begin(Pins::SD_CS);
  }
  if (!sdOk)
    return false;

  File f = SD.open(Config::CSV_FILE, FILE_APPEND);
  if (!f)
  {
    sdOk = false;
    return false;
  }
  if (f.size() == 0)
  {
    f.print(Logger::csv_header());
    f.print('\n');
  }
  f.print(line);
  f.print('\n');
  f.close();
  return true;
}

void tLogger(void *)
{
  static Snapshot s;
  static char line[256];

  for (;;)
  {
    beat(Constants::H_LOGGER);
    if (xQueueReceive(q_log, &s, pdMS_TO_TICKS(1000)) != pdTRUE)
      continue;
    Logger::format_csv(s, line, sizeof(line));
    if (!saveLine(line))
      Serial.println("SD: no se pudo registrar la fila");
  }
}

// MQTT y reenvio diferido
struct MqttSession
{
  bool wifiStarted;
  uint32_t wifiMs, brokerMs;      // ultimo intento de conexion
  uint32_t offset, savedOffset;   // posicion en el CSV de lo ya reenviado (y lo guardado en NVS)
  uint32_t resendMs, saveMs;
};

// Devuelve true si hay WiFi y broker. Si no, hace un intento de conexion y espera un poco.
static bool networkReady(MqttSession &s)
{
  if (noNetwork) // perdida de red simulada (E11)
  {
    if (mqtt.connected())
      mqtt.disconnect();
    if (WiFi.status() == WL_CONNECTED)
      WiFi.disconnect(false);
    s.wifiStarted = false;
    netOk = false;
    vTaskDelay(pdMS_TO_TICKS(500));
    return false;
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    netOk = false;
    if (!s.wifiStarted || millis() - s.wifiMs > 15000UL)
    {
      WiFi.mode(WIFI_STA);
      WiFi.begin(Config::Network::SSID, Config::Network::PASSWORD, Config::Network::CHANNEL);
      s.wifiStarted = true;
      s.wifiMs = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(250));
    return false;
  }

  if (!mqtt.connected())
  {
    netOk = false;
    if (millis() - s.brokerMs < 5000UL)
    {
      vTaskDelay(pdMS_TO_TICKS(200));
      return false;
    }
    s.brokerMs = millis();
    if (!mqtt.connect(Config::MQTT::CLIENT_ID))
    {
      Serial.println("MQTT: sin conexion con el broker");
      return false;
    }
    Serial.println("MQTT: conectado");
  }
  netOk = true;
  return true;
}

static void publishTelemetry()
{
  static char json[704];
  Snapshot s;
  if (xQueueReceive(q_mqtt, &s, 0) != pdTRUE)
    return;
  Logger::format_json(s, json, sizeof(json));
  if (strlen(json) < sizeof(json) - 2) // no se publica un JSON truncado
    mqtt.publish(Config::MQTT::TELEMETRY_TOPIC, json);
}

static void publishAlerts()
{
  Utils::Alert a;
  while (xQueueReceive(q_alert, &a, 0) == pdTRUE)
  {
    char stamp[24], buf[256];
    Time::date_text(a.epoch, stamp, sizeof(stamp));
    snprintf(buf, sizeof(buf), "{\"fecha\":\"%s\",\"codigo\":\"%s\",\"mensaje\":\"%s\"}", stamp,
             Constants::alert_code(a.code), Constants::alert_message(a.code));
    mqtt.publish(Config::MQTT::ALERTS_TOPIC, buf);
  }
}

// Publica hasta 10 filas de la SD que aun no se han enviado y devuelve el nuevo desplazamiento
static uint32_t resendPending(uint32_t off)
{
  const int BATCH = 10;
  if (!mqtt.connected() || !sdOk)
    return off;

  String lines[BATCH];
  int n = 0;
  {
    Lock lock(mtxSD, 500);
    if (!lock)
      return off;
    File f = SD.open(Config::CSV_FILE, FILE_READ);
    if (f)
    {
      if (off > f.size()) // el archivo se reemplazo
        off = 0;
      if (off == 0 && f.size() > 0) // salta el encabezado
        off = f.readStringUntil('\n').length() + 1;
      f.seek(off);
      while (n < BATCH && f.available())
        lines[n++] = f.readStringUntil('\n');
      f.close();
    }
  }

  for (int i = 0; i < n; i++)
  {
    if (lines[i].length() > 0 && !mqtt.publish(Config::MQTT::REGISTRY_TOPIC, lines[i].c_str()))
      break;
    off += lines[i].length() + 1;
  }
  return off;
}

// Reenvia filas pendientes cada 500 ms y guarda el avance en NVS como mucho cada 30 s
static void resendAndSave(MqttSession &s)
{
  if (millis() - s.resendMs < 500UL)
    return;
  s.resendMs = millis();
  s.offset = resendPending(s.offset);

  if (s.offset == s.savedOffset || millis() - s.saveMs <= 30000UL)
    return;
  Lock lock(mtxNVS, 500);
  if (!lock)
    return;
  prefs.putUInt("off", s.offset);
  s.savedOffset = s.offset;
  s.saveMs = millis();
}

void tMQTT(void *)
{
  mqtt.setServer(Config::MQTT::HOST, Config::MQTT::PORT);
  mqtt.setBufferSize(768);
  mqtt.setSocketTimeout(3);

  MqttSession session = MqttSession();
  session.brokerMs = millis() - 5000UL; // permite el primer intento de inmediato
  {
    Lock lock(mtxNVS, 500);
    if (lock)
      session.offset = prefs.getUInt("off", 0);
  }
  session.savedOffset = session.offset;

  for (;;)
  {
    beat(Constants::H_MQTT);
    if (!networkReady(session))
      continue;

    mqtt.loop();
    publishTelemetry();
    publishAlerts();
    resendAndSave(session);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// Interfaz local
// tUI: pulsadores, zumbador y pantalla
void tUI(void *)
{
  static Snapshot s;
  bool hasData = false;
  int page = 0;
  bool nextPrev = false, okPrev = false, okLong = false;
  uint32_t okPressMs = 0, lastDraw = 0;

  for (;;)
  {
    beat(Constants::H_UI);

    bool next = digitalRead(Pins::BTN_NEXT) == LOW;
    bool ok = digitalRead(Pins::BTN_OK) == LOW;
    bool changed = false;

    if (next && !nextPrev) // SIG: pagina siguiente
    {
      page = (page + 1) % Display::N_PAGES;
      changed = true;
    }
    if (ok && !okPrev)
    {
      okPressMs = millis();
      okLong = false;
    }
    if (ok && okPrev && !okLong && millis() - okPressMs >= 1500UL)
    {
      okLong = true;
      snooze = true; // OK largo: silencia la alarma
    }
    if (!ok && okPrev && !okLong && Display::isStagePage(page))
    {
      nextStage(Display::cropOfPage(page)); // OK corto en las paginas de etapa
      changed = true;
    }
    nextPrev = next;
    okPrev = ok;

    if (xQueueReceive(q_ui, &s, 0) == pdTRUE)
      hasData = true;

    // Zumbador: pitido corto cada 2 s mientras haya alarma sin silenciar
    buzzer(alarmActive && (millis() % 2000UL) < 200UL);

    if (changed || millis() - lastDraw >= Timing::UI_REFRESH_MS)
    {
      lastDraw = millis();
      Display::Status status = {sdOk, netOk, alarmActive, snooze};
      Lock lock(mtxI2C, 300);
      if (lock)
        Display::draw(oled, s, hasData, page, status);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Tareas y vigilante
// Una fila por tarea, en el orden de Constants::Heartbeat. maxSilenceMs es el
// tiempo sin latidos tras el cual el vigilante reinicia el sistema.
struct TaskSpec
{
  TaskFunction_t run;
  const char *name;
  uint32_t stackBytes;
  UBaseType_t priority;
  BaseType_t core; // 1 = medicion y control | 0 = comunicaciones e interfaz
  uint32_t maxSilenceMs;
};

static const TaskSpec TASKS[Constants::H_N] = {
    {tRain, "tLluvia", 2048, 5, 1, 5000UL},
    {tWater, "tAgua", 3072, 3, 1, Timing::staleAfter(Timing::WATER_PERIOD_MS)},
    {tControl, "tControl", 6144, 3, 1, 2 * Timing::CONTROL_PERIOD_MS + 5000UL},
    {tWeather, "tClima", 4096, 2, 1, Timing::staleAfter(Timing::WEATHER_PERIOD_MS)},
    {tLogger, "tLogger", 6144, 1, 0, 10000UL},
    {tMQTT, "tMQTT", 8192, 1, 0, 30000UL},
    {tUI, "tUI", 5120, 1, 0, 5000UL}};

static void startTasks()
{
  for (int i = 0; i < Constants::H_N; i++)
    beat(i);
  for (int i = 0; i < Constants::H_N; i++)
    xTaskCreatePinnedToCore(TASKS[i].run, TASKS[i].name, TASKS[i].stackBytes, NULL, TASKS[i].priority,
                            NULL, TASKS[i].core);
}

static void checkHeartbeats()
{
  for (int i = 0; i < Constants::H_N; i++)
  {
    if (millis() - heartbeats[i] <= TASKS[i].maxSilenceMs)
      continue;
    Serial.printf("VIGILANTE: %s sin latidos, se reinicia el sistema\n", TASKS[i].name);
    write595(0); // valvulas y bomba cerradas antes de reiniciar
    delay(200);
    ESP.restart();
  }
}

// Arranque
static void initPins()
{
  // Primero se fijan las salidas en estado seguro (todo cerrado)
  pinMode(Pins::SR_DATA, OUTPUT);
  pinMode(Pins::SR_CLK, OUTPUT);
  pinMode(Pins::SR_LATCH, OUTPUT);
  write595(0);

  pinMode(Pins::BTN_NEXT, INPUT_PULLUP);
  pinMode(Pins::BTN_OK, INPUT_PULLUP);
  pinMode(Pins::RAIN, INPUT_PULLUP);
  pinMode(Pins::LAYER_TRIG, OUTPUT);
  pinMode(Pins::LAYER_ECHO, INPUT);
  pinMode(Pins::TANK_TRIG, OUTPUT);
  pinMode(Pins::TANK_ECHO, INPUT);
  pinMode(Pins::BUZZER, OUTPUT);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(Pins::BUZZER, 2000, 8);
#else
  ledcSetup(0, 2000, 8);
  ledcAttachPin(Pins::BUZZER, 0);
#endif
  buzzer(false);
}

static void initQueues()
{
  mtxI2C = xSemaphoreCreateMutex();
  mtxSD = xSemaphoreCreateMutex();
  mtxNVS = xSemaphoreCreateMutex();

  q_pulse = xQueueCreate(16, sizeof(uint32_t));
  q_rain = xQueueCreate(16, sizeof(float));
  q_weather = xQueueCreate(1, sizeof(Utils::WeatherMeasure));
  q_water = xQueueCreate(1, sizeof(Utils::WaterMeasure));
  q_log = xQueueCreate(4, sizeof(Snapshot));
  q_ui = xQueueCreate(1, sizeof(Snapshot));
  q_mqtt = xQueueCreate(1, sizeof(Snapshot));
  q_alert = xQueueCreate(8, sizeof(Utils::Alert));
}

// Fija el origen del tiempo simulado a partir del reloj de tiempo real
static void initClock()
{
  const DateTime initial(2026, 11, 8, 6, 0, 0);
  if (!rtc.begin())
  {
    Serial.println("RTC: no responde, se usa la fecha inicial");
    epoch0 = initial.unixtime();
  }
  else
  {
    if (Config::Hardware::FORCE_DATE || !rtc.isrunning() || rtc.now().year() < 2025)
      rtc.adjust(initial);
    epoch0 = rtc.now().unixtime();
  }
  millis0 = millis();
}

static void initPeripherals()
{
  bmpOk = bmp.begin();
  if (!bmpOk)
    Serial.println("BMP180: no responde (se activara el modo seguro)");
  dht.begin();

  if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    Display::splash(oled);
  else
    Serial.println("OLED: no responde");

  SPI.begin(Pins::SD_SCK, Pins::SD_MISO, Pins::SD_MOSI, Pins::SD_CS);
  sdOk = SD.begin(Pins::SD_CS);
  Serial.println(sdOk ? "SD: lista" : "SD: no disponible");
}

void setup()
{
  initPins();

  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Estacion agrometeorologica - cafe, frijol y arroz (El Salvador) ===");

  initQueues();

  // I2C a 100 kHz porque el DS1307 no admite mas
  Wire.begin(Pins::I2C_SDA, Pins::I2C_SCL);
  Wire.setClock(100000);

  initClock();
  initPeripherals();
  loadStages();
  attachInterrupt(digitalPinToInterrupt(Pins::RAIN), isrRain, FALLING);

  char stamp[24];
  Time::date_text(epoch0, stamp, sizeof(stamp));
  Serial.printf("Fecha inicial: %s | ACEL = %lu\n", stamp, (unsigned long)Config::TIME_ACCEL);
  Serial.println(Logger::csv_header());

  startTasks();
}

static void readSerial()
{
  while (Serial.available())
  {
    char c = Serial.read();
    if (c == 'x')
    {
      Serial.println("CMD: tControl se cuelga (prueba E12); el vigilante reiniciara el sistema");
      hangControl = true;
    }
    else if (c == 'w')
    {
      noNetwork = !noNetwork;
      Serial.println(noNetwork ? "CMD: red desconectada (E11)" : "CMD: red restablecida");
    }
    else if (c == '?')
    {
      Serial.println("Comandos: x = colgar tControl | w = perder/recuperar red");
    }
  }
}

void loop()
{
  static uint32_t lastCheck = 0;

  readSerial();
  if (millis() - lastCheck >= 1000UL)
  {
    lastCheck = millis();
    checkHeartbeats();
  }
  delay(50);
}