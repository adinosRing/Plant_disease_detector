// Smoke / gas + temperature hazard monitor for ESP32.
//
//   MQ2  -> smoke / combustible-gas index (ppm, smoke-equivalent)
//   DHT11 -> temperature (absolute + rate of rise) and humidity
//   Buzzer -> intermittent chirp on WARNING, ISO 8201 "temporal-3" on ALARM
//   AWS Lambda -> signed HTTPS event for every level change, sensor fault,
//                 and a reminder every few minutes while an ALARM persists.
//
// Libraries: "DHT sensor library" + "Adafruit Unified Sensor" (Adafruit).
// Board: any ESP32 dev board with the Arduino-ESP32 core 2.x or 3.x.
// This is a hobby/early-warning device, NOT a certified smoke or gas alarm.

#include <DHT.h>
#include <Preferences.h>
#include <WiFi.h>
#include <math.h>
#include <time.h>

#include "config.h"
#include "hazard_logic.h"
#include "mq2_math.h"
#include "notifier.h"
#include "secrets.h"

using hazard::Level;

// ----------------------------------------------------------------- State
DHT dht(PIN_DHT, DHT_SENSOR_TYPE);
Preferences prefs;
hazard::Evaluator evaluator;

uint32_t bootId = 0;
uint32_t eventSeq = 0;
uint32_t bootMs = 0;
uint32_t lastSampleMs = 0;
uint32_t lastReminderMs = 0;

float r0KOhm = 0.0f;          // MQ2 clean-air resistance
bool calibrating = false;
float calSum = 0.0f;
int calCount = 0;

int dhtFailures = 0;
bool dhtFault = false;
bool gasFault = false;

// Last readings (NAN when unavailable) for event payloads.
float lastTempC = NAN, lastHumidity = NAN, lastGasPpm = NAN, lastGasRatio = NAN;
float lastRise = NAN;
uint16_t lastReasons = 0;

// --------------------------------------------------------------- Buzzer
// Patterns are alternating on/off durations in ms, starting with "on".
const uint16_t kWarnPattern[]  = {150, 1850};                           // chirp every 2 s
const uint16_t kAlarmPattern[] = {500, 500, 500, 500, 500, 1500};       // temporal-3
const uint16_t* buzzPattern = nullptr;
size_t buzzLen = 0, buzzStep = 0;
uint32_t buzzStepStart = 0;
bool buzzOn = false;
uint32_t silencedUntil = 0;   // 0 = not silenced

void buzzerWrite(bool on) {
  if (on == buzzOn) return;
  buzzOn = on;
#if BUZZER_PASSIVE
  if (on) tone(PIN_BUZZER, BUZZER_TONE_HZ); else noTone(PIN_BUZZER);
#else
  digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
#endif
#if PIN_STATUS_LED >= 0
  digitalWrite(PIN_STATUS_LED, on ? HIGH : LOW);
#endif
}

void setBuzzerForLevel(Level level) {
  const uint16_t* p = nullptr;
  size_t n = 0;
  if (level == Level::ALARM) { p = kAlarmPattern; n = sizeof kAlarmPattern / sizeof *kAlarmPattern; }
  else if (level == Level::WARNING) { p = kWarnPattern; n = sizeof kWarnPattern / sizeof *kWarnPattern; }
  if (p == buzzPattern) return;
  buzzPattern = p;
  buzzLen = n;
  buzzStep = 0;
  buzzStepStart = millis();
  buzzerWrite(p != nullptr);
}

void serviceBuzzer(uint32_t now) {
  if (silencedUntil && (int32_t)(now - silencedUntil) >= 0) silencedUntil = 0;
  if (!buzzPattern || silencedUntil) { buzzerWrite(false); return; }
  if (now - buzzStepStart >= buzzPattern[buzzStep]) {
    buzzStep = (buzzStep + 1) % buzzLen;
    buzzStepStart = now;
  }
  buzzerWrite(buzzStep % 2 == 0);
}

// ------------------------------------------------------------ Button
bool buttonPressed() {
#if PIN_SILENCE_BTN >= 0
  static bool last = false;
  static uint32_t changedAt = 0;
  bool down = digitalRead(PIN_SILENCE_BTN) == LOW;
  uint32_t now = millis();
  if (down != last && now - changedAt > 50) {  // debounce
    last = down;
    changedAt = now;
    return down;  // report the press edge
  }
#endif
  return false;
}

// ------------------------------------------------------------ Events
void appendJsonFloat(char* buf, size_t len, const char* key, float v, int decimals, bool comma) {
  size_t used = strlen(buf);
  if (isnan(v)) snprintf(buf + used, len - used, "\"%s\":null%s", key, comma ? "," : "");
  else snprintf(buf + used, len - used, "\"%s\":%.*f%s", key, decimals, v, comma ? "," : "");
}

// type: BOOT | LEVEL_CHANGED | REMINDER | SENSOR_FAULT | SENSOR_RECOVERED
void sendEvent(const char* type, Level level, Level previous, const char* sensor) {
  char reasons[96];
  hazard::reasonsToString(lastReasons, reasons, sizeof reasons);
  char reasonsJson[128] = "";
  size_t j = 0;
  if (reasons[0]) {
    reasonsJson[j++] = '"';
    for (const char* c = reasons; *c && j < sizeof reasonsJson - 3; ++c) {
      if (*c == ',') { reasonsJson[j++] = '"'; reasonsJson[j++] = ','; reasonsJson[j++] = '"'; }
      else reasonsJson[j++] = *c;
    }
    reasonsJson[j++] = '"';
    reasonsJson[j] = '\0';
  }

  char observed[16] = "null";
  if (notifier::timeSynced()) snprintf(observed, sizeof observed, "%ld", (long)time(nullptr));

  char body[notifier::kMaxBody];
  int n = snprintf(body, sizeof body,
      "{\"schema\":1,\"event_id\":\"%s-%08lx-%lu\",\"device_id\":\"%s\","
      "\"type\":\"%s\",\"level\":\"%s\",\"previous_level\":\"%s\","
      "\"reasons\":[%s],\"sensor\":%s%s%s,\"observed_at\":%s,"
      "\"uptime_ms\":%lu,\"firmware\":\"%s\",\"readings\":{",
      DEVICE_ID, (unsigned long)bootId, (unsigned long)++eventSeq, DEVICE_ID,
      type, hazard::levelName(level), hazard::levelName(previous),
      reasonsJson, sensor ? "\"" : "", sensor ? sensor : "null", sensor ? "\"" : "",
      observed, (unsigned long)millis(), FIRMWARE_VERSION);
  if (n < 0 || n >= (int)sizeof body - 200) { Serial.println("[event] too large"); return; }
  appendJsonFloat(body, sizeof body, "temperature_c", lastTempC, 1, true);
  appendJsonFloat(body, sizeof body, "humidity_pct", lastHumidity, 1, true);
  appendJsonFloat(body, sizeof body, "gas_ppm", lastGasPpm, 0, true);
  appendJsonFloat(body, sizeof body, "gas_ratio", lastGasRatio, 2, true);
  appendJsonFloat(body, sizeof body, "temp_rise_c_per_min", lastRise, 1, false);
  strncat(body, "}}", sizeof body - strlen(body) - 1);

  Serial.printf("[event] %s\n", body);
  if (!notifier::enqueue(body)) Serial.println("[event] enqueue failed");
}

// ------------------------------------------------------------ Sensors
// Returns MQ2 Rs in kOhm, or <0 if the reading is out of range (wiring fault).
float readMq2Rs() {
  uint32_t mv = 0;
  for (int i = 0; i < MQ2_ADC_OVERSAMPLE; ++i) mv += analogReadMilliVolts(PIN_MQ2_AO);
  float vPin = (mv / (float)MQ2_ADC_OVERSAMPLE) / 1000.0f;
  float vSensor = vPin * MQ2_DIVIDER_FACTOR;
  return mq2::sensorResistanceKOhm(vSensor, MQ2_VCC_VOLTS, MQ2_RLOAD_KOHM);
}

void startCalibration() {
  calibrating = true;
  calSum = 0;
  calCount = 0;
  Serial.println("[mq2] calibrating R0 - keep the sensor in clean air");
}

void sampleAndEvaluate(uint32_t now) {
  hazard::Sample s;
  s.tMs = now;

  // --- DHT11
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {
    if (++dhtFailures >= DHT_FAULT_AFTER && !dhtFault) {
      dhtFault = true;
      lastTempC = lastHumidity = NAN;
      sendEvent("SENSOR_FAULT", evaluator.level(), evaluator.level(), "DHT");
    }
  } else {
    dhtFailures = 0;
    s.tempValid = true;
    s.tempC = lastTempC = t;
    lastHumidity = h;
    if (dhtFault) {
      dhtFault = false;
      sendEvent("SENSOR_RECOVERED", evaluator.level(), evaluator.level(), "DHT");
    }
  }

  // --- MQ2
  bool warm = now - bootMs >= MQ2_WARMUP_MS;
  float rs = readMq2Rs();
  bool rsOk = rs > 0;
  if (warm) {
    if (!rsOk && !gasFault) {
      gasFault = true;
      lastGasPpm = lastGasRatio = NAN;
      sendEvent("SENSOR_FAULT", evaluator.level(), evaluator.level(), "MQ2");
    } else if (rsOk && gasFault) {
      gasFault = false;
      sendEvent("SENSOR_RECOVERED", evaluator.level(), evaluator.level(), "MQ2");
    }
    if (rsOk && calibrating) {
      calSum += rs;
      if (++calCount >= MQ2_CAL_SAMPLES) {
        r0KOhm = mq2::r0FromCleanAirRs(calSum / calCount);
        prefs.putFloat("r0", r0KOhm);
        calibrating = false;
        Serial.printf("[mq2] R0 = %.2f kOhm (saved)\n", r0KOhm);
      }
    } else if (rsOk && r0KOhm > 0) {
      s.gasValid = true;
      lastGasRatio = rs / r0KOhm;
      s.gasPpm = lastGasPpm = mq2::ppmFromRatio(lastGasRatio);
    }
  }

  // --- Evaluate
  hazard::Assessment a = evaluator.update(s);
  lastRise = a.riseValid ? a.riseCPerMin : NAN;
  lastReasons = a.reasons;

  Serial.printf("T=%.1fC H=%.0f%% rise=%.1fC/min gas=%.0fppm ratio=%.2f -> %s%s\n",
                lastTempC, lastHumidity, lastRise, lastGasPpm, lastGasRatio,
                hazard::levelName(a.level),
                !warm ? " (MQ2 warming up)" : calibrating ? " (MQ2 calibrating)" : "");

  if (a.changed) {
    if (a.level > a.previous) silencedUntil = 0;  // escalation cancels a hush
    setBuzzerForLevel(a.level);
    sendEvent("LEVEL_CHANGED", a.level, a.previous, nullptr);
    lastReminderMs = now;
  } else if (a.level == Level::ALARM && now - lastReminderMs >= ALARM_REMINDER_MS) {
    sendEvent("REMINDER", a.level, a.level, nullptr);
    lastReminderMs = now;
  }
}

// ------------------------------------------------------------ Arduino
void setup() {
  Serial.begin(115200);
  delay(200);
  bootMs = millis();
  bootId = esp_random();

  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
#if PIN_STATUS_LED >= 0
  pinMode(PIN_STATUS_LED, OUTPUT);
#endif
#if PIN_SILENCE_BTN >= 0
  pinMode(PIN_SILENCE_BTN, INPUT_PULLUP);
#endif
  analogReadResolution(12);
  dht.begin();

  // Short self-test beep so a dead buzzer is noticed at install time.
  buzzerWrite(true); delay(150); buzzerWrite(false);

  prefs.begin("hazard", false);
  bool forceCal = false;
#if PIN_SILENCE_BTN >= 0
  if (digitalRead(PIN_SILENCE_BTN) == LOW) {
    delay(3000);
    forceCal = digitalRead(PIN_SILENCE_BTN) == LOW;
  }
#endif
  if (MQ2_R0_KOHM > 0) r0KOhm = MQ2_R0_KOHM;
  else if (!forceCal) r0KOhm = prefs.getFloat("r0", 0.0f);
  if (r0KOhm <= 0) startCalibration();
  else Serial.printf("[mq2] R0 = %.2f kOhm\n", r0KOhm);

  hazard::Thresholds th;
  th.gasWarnPpm = GAS_WARN_PPM;
  th.gasAlarmPpm = GAS_ALARM_PPM;
  th.tempWarnC = TEMP_WARN_C;
  th.tempAlarmC = TEMP_ALARM_C;
  th.riseWarnCPerMin = RISE_WARN_C_PER_MIN;
  th.riseAlarmCPerMin = RISE_ALARM_C_PER_MIN;
  evaluator = hazard::Evaluator(th);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);  // UTC; needed for signatures

  if (!notifier::begin()) Serial.println("[notify] failed to start task");
  sendEvent("BOOT", Level::NORMAL, Level::NORMAL, nullptr);
  Serial.printf("[boot] %s fw %s, MQ2 warm-up %lus\n", DEVICE_ID, FIRMWARE_VERSION,
                (unsigned long)(MQ2_WARMUP_MS / 1000));
}

void loop() {
  uint32_t now = millis();

  if (buttonPressed() && buzzPattern) {
    silencedUntil = now + SILENCE_MS;
    if (silencedUntil == 0) silencedUntil = 1;
    Serial.println("[buzzer] silenced");
  }

  if (now - lastSampleMs >= SAMPLE_PERIOD_MS) {
    lastSampleMs = now;
    sampleAndEvaluate(now);
  }

  serviceBuzzer(millis());
  delay(10);
}
