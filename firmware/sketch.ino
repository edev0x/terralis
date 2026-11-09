// =============================================================================
// Estacion agrometeorologica y control de riego por parcelas con alerta
// fitosanitaria: cafe, frijol y arroz en El Salvador.
//
// Plataforma: ESP32 (wokwi-esp32-devkit-v1) + FreeRTOS
// Archivos:   pins.h (pines) | config.h (parametros) | logica.h (algoritmos,
//             probados en PC con test_logica.cpp) | este archivo (hardware y tareas)
//
// Tareas (nucleo 1 = medicion y control, nucleo 0 = comunicaciones e interfaz)
//   tLluvia   evento  prio 5   pluviometro por interrupcion
//   tAgua     10 s    prio 3   humedad de suelo, lamina del arroz, nivel del reservorio
//   tControl  10 s    prio 3   balance hidrico, bloqueos, FSM de riego, salidas, riesgo
//   tClima    60 s    prio 2   DHT22, BMP180, hoja, luz
//   tLogger   evento  prio 1   registro CSV en la microSD
//   tMQTT     60 s    prio 1   telemetria, alertas y reenvio diferido desde la SD
//   tUI       200 ms  prio 1   pantalla OLED, pulsadores y zumbador
//
// Comandos por el monitor serie:  x = colgar tControl (prueba E12)
//                                 w = simular/restablecer perdida de red (E11)
//                                 ? = ayuda
// =============================================================================
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <DHT.h>
#include <Adafruit_BMP085.h>   // el BMP180 usa esta biblioteca
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RTClib.h>

#include "pins.h"
#include "config.h"
#include "logica.h"

using namespace lg;

// ---------------------------------------------------------------- Opciones de montaje
#ifndef RAD_INVERTIDA
#define RAD_INVERTIDA 0        // 1 si el modulo LDR da mas voltaje con menos luz
#endif
#ifndef RELE_ACTIVO_BAJO
#define RELE_ACTIVO_BAJO 0     // 1 si los modulos de rele se activan con nivel bajo
#endif
#ifndef FORZAR_FECHA
#define FORZAR_FECHA 0         // 1 fuerza 2026-11-08 06:00 al arrancar (epoca seca)
#endif

// ---------------------------------------------------------------- Periodos reales (ms)
#define MAXU(a, b) ((a) > (b) ? (a) : (b))
#define PER_CLIMA_MS MAXU(T_CLIMA_MS / ACEL, 2500UL)   // el DHT22 exige >= 2,5 s
#define PER_AGUA_MS MAXU(T_AGUA_MS / ACEL, 1500UL)     // 2 sensores x 5 medidas
#define PER_CONTROL_MS MAXU(T_CONTROL_MS / ACEL, 250UL)

// ---------------------------------------------------------------- Mensajes entre tareas
struct MedClima {
  float t, hr, p, rad, hoja;  // hoja y rad en %
  bool ok;
};
struct MedAgua {
  float suelo_c, suelo_f;     // % de humedad
  float lamina;               // cm
  float reserv;               // %
  bool lam_ok, tan_ok;
};
struct Alerta {
  uint8_t cod;
  uint32_t epoch;
};

// ---------------------------------------------------------------- Objetos globales
DHT dht(PIN_DHT, DHT22);
Adafruit_BMP085 bmp;
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
RTC_DS1307 rtc;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;

SemaphoreHandle_t mtxI2C, mtxSD, mtxNVS;
QueueHandle_t qPulso, qLluvia, qClima, qAgua, qLog, qUI, qMqtt, qAlerta;

// Estado compartido (escritura por una sola tarea, lectura por otras)
volatile uint8_t etapa[3] = {0, 1, 1};  // cafe, frijol, arroz
volatile bool silenciar = false;
volatile bool alarmaActiva = false;
volatile bool colgarControl = false;    // comando 'x'
volatile bool sinRed = false;           // comando 'w'
volatile bool sdOk = false;
volatile bool netOk = false;
volatile bool bmpOk = false;

// Latidos de las tareas para el vigilante por software
enum { H_LLUVIA, H_AGUA, H_CONTROL, H_CLIMA, H_LOGGER, H_MQTT, H_UI, H_N };
volatile uint32_t hb[H_N];
static const uint32_t hbMax[H_N] = {
    5000UL,                              // tLluvia
    3 * PER_AGUA_MS + 5000UL,            // tAgua
    2 * PER_CONTROL_MS + 5000UL,         // tControl
    3 * PER_CLIMA_MS + 5000UL,           // tClima
    10000UL,                             // tLogger
    30000UL,                             // tMQTT
    5000UL};                             // tUI
static const char *const hbNombre[H_N] = {"tLluvia", "tAgua", "tControl", "tClima",
                                          "tLogger", "tMQTT", "tUI"};

// ---------------------------------------------------------------- Tiempo simulado
static uint32_t epoch0 = 0;
static uint32_t ms0 = 0;

uint32_t simEpoch() {
  return epoch0 + (uint32_t)(((uint64_t)(millis() - ms0) * ACEL) / 1000ULL);
}

// ---------------------------------------------------------------- Utilidades de hardware
void escribir595(uint8_t logico) {
  uint8_t fisico = logico;
#if RELE_ACTIVO_BAJO
  fisico ^= 0x1F;  // bits 0-4 son reles; los LED (5-7) siempre son activo alto
#endif
  digitalWrite(PIN_595_LATCH, LOW);
  shiftOut(PIN_595_DATA, PIN_595_CLK, MSBFIRST, fisico);
  digitalWrite(PIN_595_LATCH, HIGH);
}

static float adcPct(int pin) { return analogRead(pin) * 100.0f / 4095.0f; }

static float distanciaCm(int trig, int echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  unsigned long us = pulseIn(echo, HIGH, 30000UL);
  if (us == 0) return NAN;
  return us / 58.0f;
}

// Mediana de 5 medidas; NAN si hay menos de 3 validas (rango del HC-SR04: 2 a 400 cm)
static float distanciaMediana(int trig, int echo) {
  float v[5];
  int n = 0;
  for (int i = 0; i < 5; i++) {
    float d = distanciaCm(trig, echo);
    if (!isnan(d) && d >= 2.0f && d <= 400.0f) v[n++] = d;
    vTaskDelay(pdMS_TO_TICKS(60));
  }
  if (n < 3) return NAN;
  return mediana(v, n);
}

static void zumbador(bool on) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(PIN_BUZZER, on ? 2000 : 0);
#else
  ledcWriteTone(0, on ? 2000 : 0);
#endif
}

static void guardarEtapa(int i) {
  if (xSemaphoreTake(mtxNVS, pdMS_TO_TICKS(500))) {
    char k[3] = {'e', (char)('0' + i), 0};
    prefs.putUChar(k, etapa[i]);
    xSemaphoreGive(mtxNVS);
  }
}

static void bloqueoTexto(uint8_t b, char *out, size_t n) {
  out[0] = 0;
  if (b & BL_LLUVIA) strlcat(out, "LL ", n);
  if (b & BL_PRESION) strlcat(out, "PR ", n);
  if (b & BL_RESERVORIO) strlcat(out, "RE ", n);
  if (b & BL_FALLA) strlcat(out, "FA ", n);
  if (!out[0]) strlcpy(out, "ninguno", n);
}

// ---------------------------------------------------------------- Pluviometro
void IRAM_ATTR isrLluvia() {
  uint32_t t = millis();
  BaseType_t despertar = pdFALSE;
  xQueueSendFromISR(qPulso, &t, &despertar);
  if (despertar) portYIELD_FROM_ISR();
}

// tLluvia: antirrebote de 50 ms y conversion de cada vuelco en 0,2 mm
void tLluvia(void *) {
  uint32_t t, ultimo = 0;
  bool primero = true;
  for (;;) {
    hb[H_LLUVIA] = millis();
    if (xQueueReceive(qPulso, &t, pdMS_TO_TICKS(1000)) == pdTRUE) {
      if (primero || (uint32_t)(t - ultimo) >= 50UL) {
        primero = false;
        ultimo = t;
        float mm = MM_POR_PULSO;
        xQueueSend(qLluvia, &mm, 0);
      }
    }
  }
}

// ---------------------------------------------------------------- Clima
void tClima(void *) {
  TickType_t ultimo = xTaskGetTickCount();
  for (;;) {
    hb[H_CLIMA] = millis();
    MedClima m;
    m.t = dht.readTemperature();
    m.hr = dht.readHumidity();
    m.p = NAN;
    if (bmpOk && xSemaphoreTake(mtxI2C, pdMS_TO_TICKS(500))) {
      m.p = bmp.readPressure() / 100.0f;
      xSemaphoreGive(mtxI2C);
    }
    m.hoja = adcPct(PIN_HOJA);
    float luz = adcPct(PIN_LDR);
    m.rad = RAD_INVERTIDA ? 100.0f - luz : luz;
    m.ok = !isnan(m.t) && m.t > -10.0f && m.t < 60.0f && !isnan(m.hr) && m.hr >= 0.0f &&
           m.hr <= 100.0f && !isnan(m.p) && m.p > 300.0f && m.p < 1100.0f;
    xQueueOverwrite(qClima, &m);

    // Con el tiempo acelerado, mantiene el RTC alineado con el tiempo simulado
    if (ACEL != 1 && xSemaphoreTake(mtxI2C, pdMS_TO_TICKS(500))) {
      rtc.adjust(DateTime(simEpoch()));
      xSemaphoreGive(mtxI2C);
    }
    vTaskDelayUntil(&ultimo, pdMS_TO_TICKS(PER_CLIMA_MS));
  }
}

// ---------------------------------------------------------------- Agua
void tAgua(void *) {
  TickType_t ultimo = xTaskGetTickCount();
  for (;;) {
    hb[H_AGUA] = millis();
    MedAgua m;
    m.suelo_c = adcPct(PIN_SUELO_CAFE);
    m.suelo_f = adcPct(PIN_SUELO_FRIJOL);

    m.lam_ok = true;
    m.lamina = NAN;
#if !LAMINA_MODELADA
    float dl = distanciaMediana(PIN_LAMINA_TRIG, PIN_LAMINA_ECHO);
    if (isnan(dl)) {
      m.lam_ok = false;
    } else {
      m.lamina = D_SUELO_CM - dl;
      if (m.lamina < 0.0f) m.lamina = 0.0f;
    }
#endif

    float dt = distanciaMediana(PIN_TANQUE_TRIG, PIN_TANQUE_ECHO);
    if (isnan(dt)) {
      m.tan_ok = false;
      m.reserv = NAN;
    } else {
      m.tan_ok = true;
      m.reserv = clampf((D_TANQUE_VACIO_CM - dt) * 100.0f / (D_TANQUE_VACIO_CM - D_TANQUE_LLENO_CM),
                        0.0f, 100.0f);
    }
    xQueueOverwrite(qAgua, &m);
    vTaskDelayUntil(&ultimo, pdMS_TO_TICKS(PER_AGUA_MS));
  }
}

// ---------------------------------------------------------------- Control
void tControl(void *) {
  RainTracker lluvia;
  lluvia.init();
  TendenciaPresion tendencia;
  tendencia.init();
  RiesgoFungico riesgo[3];
  for (int i = 0; i < 3; i++) riesgo[i].init(RF_MEDIO[i], RF_ALTO[i]);

  Balance bc = {0.0f, TAW_CAFE, P_CAFE};
  Balance bf = {0.0f, TAW_FRIJOL, P_FRIJOL};
  LineaRiego lc, lf;
  lc.init();
  lf.init();
  uint8_t estArroz = LM_EN_BANDA;
  float lamina = LAMINA_INICIAL_CM;

  MedClima mc = {NAN, NAN, NAN, NAN, 0.0f, false};
  MedAgua ma = {0, 0, NAN, NAN, true, true};
  bool tieneClima = false, tieneAgua = false;
  int malClima = 0, malLamina = 0, malTanque = 0;
  uint32_t ultClimaMs = millis(), ultAguaMs = millis();

  float et0 = ET0_INICIAL;
  float tmaxDia = -100.0f, tminDia = 100.0f;
  float lluviaHoy = 0.0f;
  int diaAct = -1;
  bool bloqReserv = false;

  uint8_t blPrev = 0, rgPrev = 0xFF;
  bool seguroPrev = false;
  uint8_t nivelPrev[3] = {0, 0, 0};
  uint8_t estC = RG_EN_RANGO, estF = RG_EN_RANGO, estA = LM_EN_BANDA;
  uint32_t ultLog = 0, ultMqtt = 0, ultSerie = 0;
  bool primeraVez = true;

  uint32_t msPrev = millis();
  TickType_t ultimo = xTaskGetTickCount();

  for (;;) {
    if (colgarControl) {            // prueba E12: la tarea deja de dar latidos
      for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    hb[H_CONTROL] = millis();

    uint32_t ms = millis();
    float dt = (ms - msPrev) * (float)ACEL / 1000.0f;   // segundos simulados
    msPrev = ms;
    uint32_t ep = simEpoch();
    Fecha f = fecha_desde_epoch(ep);

    // ---- 1. Datos nuevos de las otras tareas
    float lluviaCiclo = 0.0f, mm;
    while (xQueueReceive(qLluvia, &mm, 0) == pdTRUE) {
      lluvia.agregar(mm, ep);
      lluviaHoy += mm;
      lluviaCiclo += mm;
    }
    MedClima nc;
    bool nuevoClima = xQueueReceive(qClima, &nc, 0) == pdTRUE;
    if (nuevoClima) {
      mc = nc;
      tieneClima = true;
      ultClimaMs = ms;
      malClima = nc.ok ? 0 : malClima + 1;
    }
    MedAgua na;
    if (xQueueReceive(qAgua, &na, 0) == pdTRUE) {
      ma = na;
      tieneAgua = true;
      ultAguaMs = ms;
      malLamina = na.lam_ok ? 0 : malLamina + 1;
      malTanque = na.tan_ok ? 0 : malTanque + 1;
    }

    // ---- 2. Cambio de dia: ET0 de Hargreaves y lluvia efectiva
    float pef = 0.0f;
    if (diaAct == -1) {
      diaAct = f.d;
    } else if (f.d != diaAct) {
      if (tmaxDia > tminDia) {
        float tmed = 0.5f * (tmaxDia + tminDia);
        et0 = et0_hargreaves(tmed, tmaxDia, tminDia,
                             ra_mm_dia(LATITUD_DEG, dia_del_anio(f.y, f.m, f.d)));
      }
      pef = pef_dia(lluviaHoy);
      lluviaHoy = 0.0f;
      tmaxDia = -100.0f;
      tminDia = 100.0f;
      diaAct = f.d;
    }

    // ---- 3. Clima: riesgo fitosanitario, presion y temperaturas del dia
    if (nuevoClima && mc.ok) {
      if (mc.t > tmaxDia) tmaxDia = mc.t;
      if (mc.t < tminDia) tminDia = mc.t;
      tendencia.muestra(mc.p, ep);
      bool mojada = mc.hoja >= HOJA_HUMEDA_PCT;
      for (int i = 0; i < 3; i++) {
        bool fav = mc.t >= RF_T_MIN[i] && mc.t <= RF_T_MAX[i] &&
                   (i == 0 ? mojada : mc.hr >= RF_HR_MIN[i]);
        riesgo[i].muestra(fav, ep / 3600u);
      }
    }
    bool hojaMojada = mc.hoja >= HOJA_HUMEDA_PCT;

    // ---- 4. Fallas de sensores (3 lecturas malas seguidas o datos vencidos)
    uint8_t fallas = 0;
    if (malClima >= 3 || (ms - ultClimaMs) > 3 * PER_CLIMA_MS + 5000UL) fallas |= FL_CLIMA;
    bool aguaVieja = (ms - ultAguaMs) > 3 * PER_AGUA_MS + 5000UL;
    if (!LAMINA_MODELADA && (malLamina >= 3 || aguaVieja)) fallas |= FL_LAMINA;
    if (malTanque >= 3 || aguaVieja) fallas |= FL_TANQUE;
    bool seguro = fallas != 0;
    bool espera = !(tieneClima && tieneAgua);

    if (espera && !seguro) {   // todavia no hay datos: todo cerrado
      escribir595(0);
      vTaskDelayUntil(&ultimo, pdMS_TO_TICKS(PER_CONTROL_MS));
      continue;
    }

    // ---- 5. Etapas y parametros
    uint8_t eC = etapa[0], eF = etapa[1], eA = etapa[2];
    if (eC >= N_ETAPAS_CAFE) eC = 0;
    if (eF >= N_ETAPAS_FRIJOL) eF = 0;
    if (eA >= N_ETAPAS_ARROZ) eA = 0;
    const ParamRiego pc = {CAFE_HUM_BAJA, CAFE_HUM_ALTA, (float)CAFE_T_MAX_S, (float)CAFE_REPOSO_S};
    const ParamRiego pf = {FRIJOL_HUM_BAJA, FRIJOL_HUM_ALTA, (float)FRIJOL_T_MAX_S,
                           (float)FRIJOL_REPOSO_S};
    const ParamLamina pl = {LAMINA_OBJ_CM[eA], LAMINA_BANDA_CM, LAMINA_MARGEN_MAX_CM};
    bool drenajeFinal = (eA == ETAPA_ARROZ_DRENAJE);
    bool llenadoHab = ARROZ_CON_RIEGO && !drenajeFinal;
    bool habCafe = (epoca(f.m, f.d) == EPOCA_SECA);   // el cafe solo se riega en epoca seca
    bool habFrijol = true;

    // ---- 6. Lamina del arroz (sensor o modelo)
#if LAMINA_MODELADA
    lamina = lamina_modelo(lamina, dt, estArroz == LM_LLENANDO && !seguro,
                           estArroz == LM_DRENANDO && !seguro, KC_ARROZ[eA] * et0, lluviaCiclo,
                           LAM_ENTRADA_CM_H, LAM_DRENAJE_CM_H, LAM_PERC_MM_DIA);
#else
    if (ma.lam_ok && !isnan(ma.lamina)) lamina = ma.lamina;
#endif

    // ---- 7. Balance hidrico de cafe y frijol
    float riegoC = (lc.est == RG_REGANDO && !seguro) ? RIEGO_TASA_CAFE_MM_H * dt / 3600.0f : 0.0f;
    float riegoF = (lf.est == RG_REGANDO && !seguro) ? RIEGO_TASA_FRIJOL_MM_H * dt / 3600.0f : 0.0f;
    balance_actualizar(bc, KC_CAFE[eC] * et0 * dt / 86400.0f, pef, riegoC, ma.suelo_c,
                       CAFE_HUM_BAJA, CAFE_HUM_ALTA);
    balance_actualizar(bf, KC_FRIJOL[eF] * et0 * dt / 86400.0f, pef, riegoF, ma.suelo_f,
                       FRIJOL_HUM_BAJA, FRIJOL_HUM_ALTA);

    // ---- 8. Bloqueos
    float ll6 = lluvia.ultimas(6, ep);
    float caida = mc.ok ? tendencia.caida(mc.p) : 0.0f;
    if (ma.tan_ok) {
      if (ma.reserv < RESERVORIO_BAJO_PCT) bloqReserv = true;
      else if (ma.reserv >= RESERVORIO_OK_PCT) bloqReserv = false;   // histeresis
    }
    uint8_t bl = 0;
    if (ll6 >= LLUVIA_BLOQUEO_MM) bl |= BL_LLUVIA;
    if (caida >= PRESION_CAIDA_HPA) bl |= BL_PRESION;
    if (bloqReserv) bl |= BL_RESERVORIO;
    if (seguro) bl |= BL_FALLA;
    bool bloqueo = bl != 0;

    // ---- 9. Turno de la bomba (una sola linea a la vez) y maquinas de estado
    bool dem[3];
    float sc[3];
    dem[0] = riego_demanda(lc, pc, ma.suelo_c, bc.D, bc.RAW(), habCafe, bloqueo);
    dem[1] = riego_demanda(lf, pf, ma.suelo_f, bf.D, bf.RAW(), habFrijol, bloqueo);
    dem[2] = lamina_demanda(estArroz, lamina, pl, llenadoHab, bloqueo);
    sc[0] = riego_score(pc, ma.suelo_c, bc.D, bc.RAW());
    sc[1] = riego_score(pf, ma.suelo_f, bf.D, bf.RAW());
    sc[2] = lamina_score(lamina, pl);
    bool hayActiva = lc.est == RG_REGANDO || lf.est == RG_REGANDO || estArroz == LM_LLENANDO;
    int turno = elegir_linea(hayActiva, dem, sc);

    riego_paso(lc, pc, ma.suelo_c, habCafe, bloqueo, turno == 0, dt);
    riego_paso(lf, pf, ma.suelo_f, habFrijol, bloqueo, turno == 1, dt);
    lamina_paso(estArroz, lamina, pl, drenajeFinal, llenadoHab, bloqueo, turno == 2);

    // ---- 10. Riesgo y alertas
    uint8_t nivel[3];
    for (int i = 0; i < 3; i++) nivel[i] = riesgo[i].nivel();
    char hora[24];
    fecha_texto(ep, hora, sizeof(hora));
    auto emitir = [&](uint8_t cod) {
      Alerta a = {cod, ep};
      xQueueSend(qAlerta, &a, 0);
      silenciar = false;
      Serial.printf("[%s] ALERTA %s: %s\n", hora, alerta_codigo(cod), alerta_mensaje(cod));
    };
    for (int i = 0; i < 3; i++)
      if (nivel[i] == 2 && nivelPrev[i] != 2) emitir(AL_RIESGO_CAFE + i);
    if ((bl & BL_RESERVORIO) && !(blPrev & BL_RESERVORIO)) emitir(AL_RESERVORIO);
    if ((bl & BL_PRESION) && !(blPrev & BL_PRESION)) emitir(AL_PRESION);
    if (seguro && !seguroPrev) emitir(AL_FALLA);
    bool algunAlto = nivel[0] == 2 || nivel[1] == 2 || nivel[2] == 2;
    alarmaActiva = (algunAlto || seguro || (bl & BL_RESERVORIO)) && !silenciar;

    // ---- 11. Salidas (74HC595). En modo seguro todo queda cerrado.
    bool vC = lc.est == RG_REGANDO, vF = lf.est == RG_REGANDO;
    bool vAin = estArroz == LM_LLENANDO, vAout = estArroz == LM_DRENANDO;
    if (seguro) vC = vF = vAin = vAout = false;
    bool bomba = vC || vF || vAin;
    bool medio = nivel[0] == 1 || nivel[1] == 1 || nivel[2] == 1;
    bool rojo = seguro || (bl & BL_RESERVORIO) || algunAlto;
    bool amarillo = !rojo && ((bl & (BL_LLUVIA | BL_PRESION)) || medio);
    bool verde = !rojo && !amarillo;
    uint8_t out = 0;
    if (bomba) out |= 1 << OUT_BOMBA;
    if (vC) out |= 1 << OUT_V_CAFE;
    if (vF) out |= 1 << OUT_V_FRIJOL;
    if (vAin) out |= 1 << OUT_V_ARROZ_IN;
    if (vAout) out |= 1 << OUT_V_ARROZ_OUT;
    if (verde) out |= 1 << OUT_LED_VERDE;
    if (amarillo) out |= 1 << OUT_LED_AMARILLO;
    if (rojo) out |= 1 << OUT_LED_ROJO;
    escribir595(out);

    // ---- 12. Informacion para las demas tareas
    Snapshot s;
    memset(&s, 0, sizeof(s));
    s.epoch = ep;
    s.t = mc.t;
    s.hr = mc.hr;
    s.p = mc.p;
    s.rad = mc.rad;
    s.lluvia_dia = lluvia.dia(ep);
    s.lluvia_6h = ll6;
    s.suelo_cafe = ma.suelo_c;
    s.suelo_frijol = ma.suelo_f;
    s.hoja = hojaMojada ? 1 : 0;
    s.lamina = lamina;
    s.lamina_obj = pl.obj;
    s.reservorio = ma.reserv;
    s.est_cafe = lc.est;
    s.est_frijol = lf.est;
    s.est_arroz = estArroz;
    for (int i = 0; i < 3; i++) {
      s.riesgo[i] = nivel[i];
      s.h48[i] = (uint8_t)riesgo[i].h48();
    }
    s.etapa[0] = eC;
    s.etapa[1] = eF;
    s.etapa[2] = eA;
    s.epoca = epoca(f.m, f.d);
    s.bloqueos = bl;
    s.fallas = fallas;
    s.seguro = seguro ? 1 : 0;
    s.salidas = out;
    s.et0 = et0;
    s.d_cafe = bc.D;
    s.d_frijol = bf.D;

    xQueueOverwrite(qUI, &s);
    if (primeraVez || ep - ultLog >= T_LOGGER_S) {
      ultLog = ep;
      xQueueSend(qLog, &s, 0);
    }
    if (primeraVez || ep - ultMqtt >= T_MQTT_MS / 1000UL) {
      ultMqtt = ep;
      xQueueOverwrite(qMqtt, &s);
    }

    // ---- 13. Mensajes por el puerto serie (cambios de estado y resumen cada minuto simulado)
    if (lc.est != estC) Serial.printf("[%s] CAFE   %s -> %s\n", hora, nombre_riego(estC), nombre_riego(lc.est));
    if (lf.est != estF) Serial.printf("[%s] FRIJOL %s -> %s\n", hora, nombre_riego(estF), nombre_riego(lf.est));
    if (estArroz != estA) Serial.printf("[%s] ARROZ  %s -> %s (lamina %.1f cm, objetivo %.1f)\n", hora, nombre_lamina(estA), nombre_lamina(estArroz), lamina, pl.obj);
    if (bl != rgPrev) {
      char txt[24];
      bloqueoTexto(bl, txt, sizeof(txt));
      Serial.printf("[%s] BLOQUEOS: %s\n", hora, txt);
    }
    if (primeraVez || ep - ultSerie >= 60) {
      ultSerie = ep;
      char fila[256];
      formatear_csv(s, fila, sizeof(fila));
      Serial.println(fila);
    }

    estC = lc.est;
    estF = lf.est;
    estA = estArroz;
    blPrev = bl;
    rgPrev = bl;
    seguroPrev = seguro;
    for (int i = 0; i < 3; i++) nivelPrev[i] = nivel[i];
    primeraVez = false;

    vTaskDelayUntil(&ultimo, pdMS_TO_TICKS(PER_CONTROL_MS));
  }
}

// ---------------------------------------------------------------- Registro en la SD
static bool guardarLinea(const char *linea) {
  static uint32_t ultReintento = 0;
  bool ok = false;
  if (!xSemaphoreTake(mtxSD, pdMS_TO_TICKS(2000))) return false;
  if (!sdOk && millis() - ultReintento > 60000UL) {
    ultReintento = millis();
    sdOk = SD.begin(PIN_SD_CS);
  }
  if (sdOk) {
    File f = SD.open(ARCHIVO_CSV, FILE_APPEND);
    if (f) {
      if (f.size() == 0) {
        f.print(csv_encabezado());
        f.print('\n');
      }
      f.print(linea);
      f.print('\n');
      f.close();
      ok = true;
    } else {
      sdOk = false;
    }
  }
  xSemaphoreGive(mtxSD);
  return ok;
}

void tLogger(void *) {
  Snapshot s;
  char linea[256];
  for (;;) {
    hb[H_LOGGER] = millis();
    if (xQueueReceive(qLog, &s, pdMS_TO_TICKS(1000)) == pdTRUE) {
      formatear_csv(s, linea, sizeof(linea));
      if (!guardarLinea(linea)) Serial.println("SD: no se pudo registrar la fila");
    }
  }
}

// ---------------------------------------------------------------- MQTT y reenvio diferido
// Publica en TOPIC_REGISTRO las filas de la SD que aun no se han enviado.
// El desplazamiento se guarda en memoria no volatil (NVS).
static uint32_t reenviarPendientes(uint32_t off) {
  if (!mqtt.connected() || !sdOk) return off;
  String lineas[10];
  uint32_t largo[10];
  int n = 0;

  if (!xSemaphoreTake(mtxSD, pdMS_TO_TICKS(500))) return off;
  File f = SD.open(ARCHIVO_CSV, FILE_READ);
  if (f) {
    if (off > f.size()) off = 0;                  // el archivo se reemplazo
    if (off == 0 && f.size() > 0) {               // salta el encabezado
      String h = f.readStringUntil('\n');
      off = h.length() + 1;
    }
    f.seek(off);
    while (n < 10 && f.available()) {
      lineas[n] = f.readStringUntil('\n');
      largo[n] = lineas[n].length() + 1;
      n++;
    }
    f.close();
  }
  xSemaphoreGive(mtxSD);

  for (int i = 0; i < n; i++) {
    if (lineas[i].length() > 0 && !mqtt.publish(TOPIC_REGISTRO, lineas[i].c_str())) break;
    off += largo[i];
  }
  return off;
}

void tMQTT(void *) {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(768);
  mqtt.setSocketTimeout(3);

  uint32_t off = 0, offGuardado = 0;
  if (xSemaphoreTake(mtxNVS, pdMS_TO_TICKS(500))) {
    off = prefs.getUInt("off", 0);
    xSemaphoreGive(mtxNVS);
  }
  offGuardado = off;

  bool wifiIniciado = false;
  uint32_t tWifi = 0, tBroker = millis() - 5000UL, tPend = 0, tGuardar = 0;
  char js[704];

  for (;;) {
    hb[H_MQTT] = millis();

    if (sinRed) {                                   // perdida de red simulada (E11)
      if (mqtt.connected()) mqtt.disconnect();
      if (WiFi.status() == WL_CONNECTED) WiFi.disconnect(false);
      wifiIniciado = false;
      netOk = false;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (WiFi.status() != WL_CONNECTED) {
      netOk = false;
      if (!wifiIniciado || millis() - tWifi > 15000UL) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
        wifiIniciado = true;
        tWifi = millis();
      }
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    if (!mqtt.connected()) {
      netOk = false;
      if (millis() - tBroker < 5000UL) {
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
      tBroker = millis();
      if (!mqtt.connect(MQTT_CLIENT_ID)) {
        Serial.println("MQTT: sin conexion con el broker");
        continue;
      }
      Serial.println("MQTT: conectado");
    }
    netOk = true;
    mqtt.loop();

    Snapshot s;
    if (xQueueReceive(qMqtt, &s, 0) == pdTRUE) {
      formatear_json(s, js, sizeof(js));
      if (strlen(js) < sizeof(js) - 2) mqtt.publish(TOPIC_TELEMETRIA, js);
    }

    Alerta a;
    while (xQueueReceive(qAlerta, &a, 0) == pdTRUE) {
      char fh[24], buf[256];
      fecha_texto(a.epoch, fh, sizeof(fh));
      snprintf(buf, sizeof(buf), "{\"fecha\":\"%s\",\"codigo\":\"%s\",\"mensaje\":\"%s\"}", fh,
               alerta_codigo(a.cod), alerta_mensaje(a.cod));
      mqtt.publish(TOPIC_ALERTAS, buf);
    }

    if (millis() - tPend >= 500UL) {
      tPend = millis();
      off = reenviarPendientes(off);
      if (off != offGuardado && millis() - tGuardar > 30000UL) {
        if (xSemaphoreTake(mtxNVS, pdMS_TO_TICKS(500))) {
          prefs.putUInt("off", off);
          xSemaphoreGive(mtxNVS);
          offGuardado = off;
          tGuardar = millis();
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ---------------------------------------------------------------- Interfaz local
#define N_PAGINAS 8

static void dibujar(const Snapshot &s, bool hay, int pag) {
  char l[40], fh[24], bq[24];
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);

  static const char *const titulos[N_PAGINAS] = {"CLIMA",   "SUELO",  "RIEGO",  "ARROZ",
                                                 "RIESGO",  "ETAPA CAFE", "ETAPA FRIJOL", "ETAPA ARROZ"};
  snprintf(l, sizeof(l), "%d/%d %s", pag + 1, N_PAGINAS, titulos[pag]);
  oled.print(l);
  oled.setCursor(104, 0);
  oled.print(sdOk ? 'S' : '-');
  oled.print(netOk ? 'R' : '-');
  oled.print(alarmaActiva ? '!' : (silenciar ? 'x' : ' '));

  if (!hay) {
    oled.setCursor(0, 16);
    oled.print("Esperando datos...");
    oled.display();
    return;
  }

  int y = 12;
  auto linea = [&](const char *txt) {
    oled.setCursor(0, y);
    oled.print(txt);
    y += 9;
  };

  switch (pag) {
    case 0:
      fecha_texto(s.epoch, fh, sizeof(fh));
      linea(fh);
      if (isnan(s.t)) snprintf(l, sizeof(l), "T --  HR --");
      else snprintf(l, sizeof(l), "T %.1fC  HR %.0f%%", s.t, s.hr);
      linea(l);
      snprintf(l, sizeof(l), "P %.0f hPa  Luz %.0f%%", s.p, s.rad);
      linea(l);
      snprintf(l, sizeof(l), "Epoca: %s", nombre_epoca(s.epoca));
      linea(l);
      snprintf(l, sizeof(l), "Hoja: %s", s.hoja ? "mojada" : "seca");
      linea(l);
      break;
    case 1:
      snprintf(l, sizeof(l), "Suelo cafe   %.0f%%", s.suelo_cafe);
      linea(l);
      snprintf(l, sizeof(l), "Suelo frijol %.0f%%", s.suelo_frijol);
      linea(l);
      snprintf(l, sizeof(l), "Lluvia dia %.1f mm", s.lluvia_dia);
      linea(l);
      snprintf(l, sizeof(l), "Lluvia 6 h %.1f mm", s.lluvia_6h);
      linea(l);
      snprintf(l, sizeof(l), "ET0 %.2f  D %.0f/%.0f", s.et0, s.d_cafe, s.d_frijol);
      linea(l);
      break;
    case 2:
      snprintf(l, sizeof(l), "Cafe:   %s", nombre_riego(s.est_cafe));
      linea(l);
      snprintf(l, sizeof(l), "Frijol: %s", nombre_riego(s.est_frijol));
      linea(l);
      snprintf(l, sizeof(l), "Bomba: %s", (s.salidas & (1 << OUT_BOMBA)) ? "ON" : "OFF");
      linea(l);
      bloqueoTexto(s.bloqueos, bq, sizeof(bq));
      snprintf(l, sizeof(l), "Bloq: %s", bq);
      linea(l);
      snprintf(l, sizeof(l), "Reservorio %.0f%%", s.reservorio);
      linea(l);
      break;
    case 3:
      snprintf(l, sizeof(l), "Lamina %.1f / %.1f cm", s.lamina, s.lamina_obj);
      linea(l);
      snprintf(l, sizeof(l), "Estado: %s", nombre_lamina(s.est_arroz));
      linea(l);
      snprintf(l, sizeof(l), "Entrada %s Drenaje %s", (s.salidas & (1 << OUT_V_ARROZ_IN)) ? "ON" : "--",
               (s.salidas & (1 << OUT_V_ARROZ_OUT)) ? "ON" : "--");
      linea(l);
      linea(NOMBRES_ETAPA_ARROZ[s.etapa[2]]);
      snprintf(l, sizeof(l), "Reservorio %.0f%%", s.reservorio);
      linea(l);
      break;
    case 4:
      snprintf(l, sizeof(l), "Cafe (roya)  %s %uh", nombre_riesgo(s.riesgo[0]), s.h48[0]);
      linea(l);
      snprintf(l, sizeof(l), "Frijol       %s %uh", nombre_riesgo(s.riesgo[1]), s.h48[1]);
      linea(l);
      snprintf(l, sizeof(l), "Arroz (anub) %s %uh", nombre_riesgo(s.riesgo[2]), s.h48[2]);
      linea(l);
      linea("Horas favorables 48 h");
      if (s.seguro) linea("MODO SEGURO!");
      break;
    default: {
      int i = pag - 5;
      const char *nom = i == 0 ? NOMBRES_ETAPA_CAFE[s.etapa[0]]
                        : i == 1 ? NOMBRES_ETAPA_FRIJOL[s.etapa[1]]
                                 : NOMBRES_ETAPA_ARROZ[s.etapa[2]];
      linea("Etapa actual:");
      linea(nom);
      linea("");
      linea("OK corto: siguiente");
      linea("OK largo: silenciar");
      break;
    }
  }
  oled.display();
}

static void cambiarEtapa(int i) {
  uint8_t n = i == 0 ? N_ETAPAS_CAFE : (i == 1 ? N_ETAPAS_FRIJOL : N_ETAPAS_ARROZ);
  etapa[i] = (etapa[i] + 1) % n;
  guardarEtapa(i);
}

void tUI(void *) {
  Snapshot s;
  memset(&s, 0, sizeof(s));
  bool hay = false;
  int pag = 0;
  bool sigPrev = false, okPrev = false, okLargo = false;
  uint32_t okT = 0, ultDibujo = 0, ultCambio = 0;

  for (;;) {
    hb[H_UI] = millis();

    bool sig = digitalRead(PIN_BTN_SIG) == LOW;
    bool ok = digitalRead(PIN_BTN_OK) == LOW;
    bool cambio = false;
    if (sig && !sigPrev) {
      pag = (pag + 1) % N_PAGINAS;
      cambio = true;
    }
    if (ok && !okPrev) {
      okT = millis();
      okLargo = false;
    }
    if (ok && okPrev && !okLargo && millis() - okT >= 1500UL) {
      okLargo = true;
      silenciar = true;          // pulsacion larga: silencia la alarma
    }
    if (!ok && okPrev && !okLargo && pag >= 5) {
      cambiarEtapa(pag - 5);     // pulsacion corta en las paginas de configuracion
      cambio = true;
    }
    sigPrev = sig;
    okPrev = ok;

    Snapshot nuevo;
    if (xQueueReceive(qUI, &nuevo, 0) == pdTRUE) {
      s = nuevo;
      hay = true;
    }

    // Zumbador: pitido corto cada 2 s mientras haya alarma sin silenciar
    zumbador(alarmaActiva && (millis() % 2000UL) < 200UL);

    if (cambio || millis() - ultDibujo >= T_UI_MS) {
      ultDibujo = millis();
      if (xSemaphoreTake(mtxI2C, pdMS_TO_TICKS(300))) {
        dibujar(s, hay, pag);
        xSemaphoreGive(mtxI2C);
      }
    }
    (void)ultCambio;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ---------------------------------------------------------------- Arranque
void setup() {
  // Primero se fijan las salidas en estado seguro (todo cerrado)
  pinMode(PIN_595_DATA, OUTPUT);
  pinMode(PIN_595_CLK, OUTPUT);
  pinMode(PIN_595_LATCH, OUTPUT);
  escribir595(0);

  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Estacion agrometeorologica - cafe, frijol y arroz (El Salvador) ===");

  pinMode(PIN_BTN_SIG, INPUT_PULLUP);
  pinMode(PIN_BTN_OK, INPUT_PULLUP);
  pinMode(PIN_LLUVIA, INPUT_PULLUP);
  pinMode(PIN_LAMINA_TRIG, OUTPUT);
  pinMode(PIN_LAMINA_ECHO, INPUT);
  pinMode(PIN_TANQUE_TRIG, OUTPUT);
  pinMode(PIN_TANQUE_ECHO, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BUZZER, 2000, 8);
#else
  ledcSetup(0, 2000, 8);
  ledcAttachPin(PIN_BUZZER, 0);
#endif
  zumbador(false);

  mtxI2C = xSemaphoreCreateMutex();
  mtxSD = xSemaphoreCreateMutex();
  mtxNVS = xSemaphoreCreateMutex();
  qPulso = xQueueCreate(16, sizeof(uint32_t));
  qLluvia = xQueueCreate(16, sizeof(float));
  qClima = xQueueCreate(1, sizeof(MedClima));
  qAgua = xQueueCreate(1, sizeof(MedAgua));
  qLog = xQueueCreate(4, sizeof(Snapshot));
  qUI = xQueueCreate(1, sizeof(Snapshot));
  qMqtt = xQueueCreate(1, sizeof(Snapshot));
  qAlerta = xQueueCreate(8, sizeof(Alerta));

  // I2C a 100 kHz porque el DS1307 no admite mas
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);

  // Reloj de tiempo real
  const DateTime inicial(2026, 11, 8, 6, 0, 0);
  if (!rtc.begin()) {
    Serial.println("RTC: no responde, se usa la fecha inicial");
    epoch0 = inicial.unixtime();
  } else {
    if (FORZAR_FECHA || !rtc.isrunning() || rtc.now().year() < 2025) rtc.adjust(inicial);
    epoch0 = rtc.now().unixtime();
  }
  ms0 = millis();

  bmpOk = bmp.begin();
  if (!bmpOk) Serial.println("BMP180: no responde (se activara el modo seguro)");
  dht.begin();

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED: no responde");
  } else {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("Estacion agromet.");
    oled.println("Cafe/Frijol/Arroz");
    oled.println("Iniciando...");
    oled.display();
  }

  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  sdOk = SD.begin(PIN_SD_CS);
  Serial.println(sdOk ? "SD: lista" : "SD: no disponible");

  prefs.begin("finca", false);
  etapa[0] = prefs.getUChar("e0", 0);
  etapa[1] = prefs.getUChar("e1", 1);
  etapa[2] = prefs.getUChar("e2", 1);
  if (etapa[0] >= N_ETAPAS_CAFE) etapa[0] = 0;
  if (etapa[1] >= N_ETAPAS_FRIJOL) etapa[1] = 0;
  if (etapa[2] >= N_ETAPAS_ARROZ) etapa[2] = 0;

  attachInterrupt(digitalPinToInterrupt(PIN_LLUVIA), isrLluvia, FALLING);

  for (int i = 0; i < H_N; i++) hb[i] = millis();

  char fh[24];
  fecha_texto(epoch0, fh, sizeof(fh));
  Serial.printf("Fecha inicial: %s | ACEL = %d\n", fh, ACEL);
  Serial.println(csv_encabezado());

  // Nucleo 1: medicion y control | Nucleo 0: comunicaciones e interfaz
  xTaskCreatePinnedToCore(tLluvia, "tLluvia", 2048, NULL, 5, NULL, 1);
  xTaskCreatePinnedToCore(tAgua, "tAgua", 3072, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(tControl, "tControl", 6144, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(tClima, "tClima", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(tLogger, "tLogger", 6144, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(tMQTT, "tMQTT", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(tUI, "tUI", 5120, NULL, 1, NULL, 0);
}

// loop(): vigilante por software y comandos de prueba
static void leerSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'x') {
      Serial.println("CMD: tControl se cuelga (prueba E12); el vigilante reiniciara el sistema");
      colgarControl = true;
    } else if (c == 'w') {
      sinRed = !sinRed;
      Serial.println(sinRed ? "CMD: red desconectada (E11)" : "CMD: red restablecida");
    } else if (c == '?') {
      Serial.println("Comandos: x = colgar tControl | w = perder/recuperar red");
    }
  }
}

void loop() {
  static uint32_t ultimo = 0;
  leerSerial();
  if (millis() - ultimo >= 1000UL) {
    ultimo = millis();
    for (int i = 0; i < H_N; i++) {
      if (millis() - hb[i] > hbMax[i]) {
        Serial.printf("VIGILANTE: %s sin latidos, se reinicia el sistema\n", hbNombre[i]);
        escribir595(0);   // valvulas y bomba cerradas antes de reiniciar
        delay(200);
        ESP.restart();
      }
    }
  }
  delay(50);
}
