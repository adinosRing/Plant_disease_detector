// Host-side tests for hazard_logic + mq2_math.  Run:  make -C firmware/test
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../hazard_monitor/hazard_logic.h"
#include "../hazard_monitor/mq2_math.h"

using hazard::Evaluator;
using hazard::Level;
using hazard::Sample;

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

static Sample mk(uint32_t t, float gas, float temp) {
  Sample s;
  s.tMs = t;
  s.gasValid = gas >= 0;
  s.gasPpm = gas;
  s.tempValid = !isnan(temp);
  s.tempC = temp;
  return s;
}

static void test_normal_stays_normal() {
  Evaluator e;
  for (uint32_t t = 0; t < 120000; t += 2000) {
    auto a = e.update(mk(t, 20, 22));
    CHECK(a.level == Level::NORMAL);
    CHECK(!a.changed);
  }
}

static void test_gas_warning_needs_confirmation() {
  Evaluator e;  // escalateSamples = 2
  auto a = e.update(mk(0, 400, 22));
  CHECK(a.level == Level::NORMAL);
  CHECK(a.rawLevel == Level::WARNING);
  a = e.update(mk(2000, 400, 22));
  CHECK(a.level == Level::WARNING && a.changed);
  CHECK(a.reasons & hazard::REASON_GAS_WARN);
}

static void test_single_spike_ignored() {
  Evaluator e;
  e.update(mk(0, 5000, 22));
  auto a = e.update(mk(2000, 20, 22));
  CHECK(a.level == Level::NORMAL);
}

static void test_gas_alarm_and_hysteresis_clear() {
  Evaluator e;
  e.update(mk(0, 1500, 22));
  auto a = e.update(mk(2000, 1500, 22));
  CHECK(a.level == Level::ALARM);
  CHECK(a.reasons & hazard::REASON_GAS_ALARM);
  // 900 ppm: below alarm (1000) but inside 20 % hysteresis band (800) -> hold.
  uint32_t t = 4000;
  for (int i = 0; i < 10; ++i, t += 2000) a = e.update(mk(t, 900, 22));
  CHECK(a.level == Level::ALARM);
  // 500 ppm: clearly WARNING; needs 5 samples to step down.
  for (int i = 0; i < 4; ++i, t += 2000) a = e.update(mk(t, 500, 22));
  CHECK(a.level == Level::ALARM);
  a = e.update(mk(t, 500, 22)); t += 2000;
  CHECK(a.level == Level::WARNING && a.changed && a.previous == Level::ALARM);
  // Back to clean air -> NORMAL after 5 samples.
  for (int i = 0; i < 5; ++i, t += 2000) a = e.update(mk(t, 20, 22));
  CHECK(a.level == Level::NORMAL && a.changed);
}

static void test_flapping_reset() {
  Evaluator e;
  e.update(mk(0, 400, 22));
  e.update(mk(2000, 400, 22));  // WARNING
  uint32_t t = 4000;
  // Alternate clean / dirty: the clear counter keeps resetting.
  for (int i = 0; i < 20; ++i, t += 2000) {
    auto a = e.update(mk(t, (i % 4 == 3) ? 400 : 20, 22));
    CHECK(a.level == Level::WARNING);
  }
}

static void test_temperature_levels() {
  Evaluator e;
  e.update(mk(0, 20, 42));
  auto a = e.update(mk(120000, 20, 42));  // far apart: no rate-of-rise
  CHECK(a.level == Level::WARNING);
  CHECK(a.reasons & hazard::REASON_TEMP_WARN);
  e.update(mk(240000, 20, 51));
  a = e.update(mk(360000, 20, 51));
  CHECK(a.level == Level::ALARM);
  CHECK(a.reasons & hazard::REASON_TEMP_ALARM);
}

static void test_rate_of_rise() {
  Evaluator e;
  // 25 C -> 35 C over 60 s (10 C/min) while still below absolute thresholds.
  Level last = Level::NORMAL;
  bool sawRise = false;
  for (uint32_t t = 0; t <= 60000; t += 2000) {
    float temp = 25.0f + 10.0f * t / 60000.0f;
    auto a = e.update(mk(t, 20, floorf(temp)));  // DHT11: whole degrees
    if (a.reasons & hazard::REASON_RISE_ALARM) sawRise = true;
    last = a.level;
  }
  CHECK(sawRise);
  CHECK(last == Level::ALARM);
}

static void test_no_rise_before_min_span() {
  Evaluator e;
  auto a = e.update(mk(0, 20, 20));
  a = e.update(mk(2000, 20, 30));  // +10 C in 2 s: not enough history
  CHECK(!a.riseValid);
  CHECK(a.level == Level::NORMAL);
}

static void test_combined_gas_and_heat_is_alarm() {
  Evaluator e;
  e.update(mk(0, 400, 41));
  auto a = e.update(mk(120000, 400, 41));
  CHECK(a.level == Level::ALARM);
  CHECK(a.reasons & hazard::REASON_COMBINED);
}

static void test_invalid_sensors_ignored() {
  Evaluator e;
  for (uint32_t t = 0; t < 20000; t += 2000) {
    auto a = e.update(mk(t, -1, NAN));
    CHECK(a.level == Level::NORMAL);
  }
}

static void test_millis_wraparound() {
  Evaluator e;
  uint32_t start = 0xFFFFFFFFu - 30000;  // wraps during the ramp
  bool alarm = false;
  for (uint32_t i = 0; i <= 30; ++i) {
    uint32_t t = start + i * 2000;
    auto a = e.update(mk(t, 20, 25.0f + i * 0.34f * 2));  // ~20 C/min
    if (a.level == Level::ALARM) alarm = true;
  }
  CHECK(alarm);
}

static void test_reason_string() {
  char buf[64];
  size_t n = hazard::reasonsToString(
      hazard::REASON_GAS_WARN | hazard::REASON_RISE_ALARM, buf, sizeof buf);
  CHECK(n == 2);
  CHECK(strcmp(buf, "GAS_WARN,RISE_ALARM") == 0);
  char tiny[10];
  n = hazard::reasonsToString(
      hazard::REASON_GAS_WARN | hazard::REASON_RISE_ALARM, tiny, sizeof tiny);
  CHECK(n == 1 && strcmp(tiny, "GAS_WARN") == 0);
}

static void test_mq2_math() {
  // Vout = 5 * 1 / (Rs + 1) with Rs = 9 kOhm -> 0.5 V
  float rs = mq2::sensorResistanceKOhm(0.5f, 5.0f, 1.0f);
  CHECK(fabsf(rs - 9.0f) < 1e-4f);
  CHECK(mq2::sensorResistanceKOhm(0.0f, 5.0f, 1.0f) < 0);
  CHECK(mq2::sensorResistanceKOhm(5.0f, 5.0f, 1.0f) < 0);
  CHECK(fabsf(mq2::r0FromCleanAirRs(9.83f) - 1.0f) < 1e-4f);
  // Reference point of the smoke curve: ratio 10^0.53 -> 200 ppm.
  CHECK(fabsf(mq2::ppmFromRatio(powf(10, 0.53f)) - 200.0f) < 0.5f);
  // Lower ratio = more smoke.
  CHECK(mq2::ppmFromRatio(1.0f) > mq2::ppmFromRatio(3.0f));
  // Clean air reads low.
  CHECK(mq2::ppmFromRatio(mq2::kCleanAirRatio) < 30.0f);
}

int main() {
  struct { const char* name; void (*fn)(); } tests[] = {
      {"normal_stays_normal", test_normal_stays_normal},
      {"gas_warning_needs_confirmation", test_gas_warning_needs_confirmation},
      {"single_spike_ignored", test_single_spike_ignored},
      {"gas_alarm_and_hysteresis_clear", test_gas_alarm_and_hysteresis_clear},
      {"flapping_reset", test_flapping_reset},
      {"temperature_levels", test_temperature_levels},
      {"rate_of_rise", test_rate_of_rise},
      {"no_rise_before_min_span", test_no_rise_before_min_span},
      {"combined_gas_and_heat_is_alarm", test_combined_gas_and_heat_is_alarm},
      {"invalid_sensors_ignored", test_invalid_sensors_ignored},
      {"millis_wraparound", test_millis_wraparound},
      {"reason_string", test_reason_string},
      {"mq2_math", test_mq2_math},
  };
  for (auto& t : tests) {
    int before = failures;
    t.fn();
    printf("%s %s\n", failures == before ? "PASS" : "FAIL", t.name);
  }
  printf("\n%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
