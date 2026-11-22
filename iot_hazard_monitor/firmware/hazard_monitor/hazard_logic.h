// Pure C++ hazard-evaluation logic (no Arduino dependencies) so it can be
// unit-tested on a desktop compiler. See ../test/test_hazard_logic.cpp.
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace hazard {

enum class Level : uint8_t { NORMAL = 0, WARNING = 1, ALARM = 2 };

// Bit flags describing *why* a level was reached.
enum Reason : uint16_t {
  REASON_NONE        = 0,
  REASON_GAS_WARN    = 1 << 0,
  REASON_GAS_ALARM   = 1 << 1,
  REASON_TEMP_WARN   = 1 << 2,
  REASON_TEMP_ALARM  = 1 << 3,
  REASON_RISE_WARN   = 1 << 4,  // rate-of-rise (fast heating)
  REASON_RISE_ALARM  = 1 << 5,
  REASON_COMBINED    = 1 << 6,  // smoke/gas + heat together => likely fire
};

struct Thresholds {
  // MQ2 smoke-equivalent concentration (ppm). Clean air reads ~10-30 ppm.
  float gasWarnPpm  = 300.0f;
  float gasAlarmPpm = 1000.0f;
  // Absolute temperature (deg C). The DHT22 reads up to 80 C; 57 C is the
  // usual rating of fixed-temperature heat detectors.
  float tempWarnC  = 45.0f;
  float tempAlarmC = 57.0f;
  // Rate of rise (deg C per minute) over `riseWindowMs`.
  float riseWarnCPerMin  = 4.0f;
  float riseAlarmCPerMin = 8.0f;
  uint32_t riseWindowMs    = 60000;  // look-back window for rate of rise
  uint32_t riseMinSpanMs   = 30000;  // need at least this much history

  // Hysteresis applied when *leaving* a level (prevents flapping).
  float gasHysteresisFrac = 0.20f;  // 20 % below the threshold
  float tempHysteresisC   = 2.0f;
  float riseHysteresisCPerMin = 1.0f;

  // Consecutive samples needed to escalate / de-escalate.
  uint8_t escalateSamples = 2;
  uint8_t clearSamples    = 5;
};

struct Sample {
  uint32_t tMs = 0;       // monotonic time (millis())
  bool  gasValid = false; // false while MQ2 heater warms up / on ADC fault
  float gasPpm = 0.0f;
  bool  tempValid = false;
  float tempC = 0.0f;
};

struct Assessment {
  Level level = Level::NORMAL;    // debounced, latched level
  Level rawLevel = Level::NORMAL; // instantaneous level of this sample
  uint16_t reasons = REASON_NONE; // reasons for the instantaneous level
  bool  riseValid = false;
  float riseCPerMin = 0.0f;
  bool  changed = false;          // `level` changed on this sample
  Level previous = Level::NORMAL;
};

const char* levelName(Level l);

// Writes comma-separated reason names into `out` (always NUL-terminated).
// Returns the number of reasons written.
size_t reasonsToString(uint16_t reasons, char* out, size_t outLen);

class Evaluator {
 public:
  explicit Evaluator(const Thresholds& t = Thresholds()) : t_(t) {}

  Assessment update(const Sample& s);

  Level level() const { return level_; }
  const Thresholds& thresholds() const { return t_; }
  void reset();

 private:
  static constexpr size_t kHistory = 64;  // >= window / sample period

  struct TempPoint { uint32_t tMs; float c; };

  void pushTemp(uint32_t tMs, float c);
  bool riseRate(uint32_t nowMs, float* cPerMin) const;
  Level classify(const Sample& s, bool riseValid, float rise, float relax,
                 uint16_t* reasons) const;

  Thresholds t_;
  Level level_ = Level::NORMAL;
  uint8_t upCount_ = 0;
  uint8_t downCount_ = 0;
  Level pendingUp_ = Level::NORMAL;

  TempPoint hist_[kHistory] = {};
  size_t histHead_ = 0;   // index of next write
  size_t histCount_ = 0;
};

}  // namespace hazard
