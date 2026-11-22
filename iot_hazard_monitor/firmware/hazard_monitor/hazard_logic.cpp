#include "hazard_logic.h"

#include <string.h>

namespace hazard {

const char* levelName(Level l) {
  switch (l) {
    case Level::NORMAL:  return "NORMAL";
    case Level::WARNING: return "WARNING";
    case Level::ALARM:   return "ALARM";
  }
  return "UNKNOWN";
}

size_t reasonsToString(uint16_t reasons, char* out, size_t outLen) {
  static const struct { uint16_t bit; const char* name; } kNames[] = {
      {REASON_GAS_WARN, "GAS_WARN"},     {REASON_GAS_ALARM, "GAS_ALARM"},
      {REASON_TEMP_WARN, "TEMP_WARN"},   {REASON_TEMP_ALARM, "TEMP_ALARM"},
      {REASON_RISE_WARN, "RISE_WARN"},   {REASON_RISE_ALARM, "RISE_ALARM"},
      {REASON_COMBINED, "COMBINED"},
  };
  if (outLen == 0) return 0;
  out[0] = '\0';
  size_t n = 0, used = 0;
  for (const auto& e : kNames) {
    if (!(reasons & e.bit)) continue;
    size_t need = strlen(e.name) + (n ? 1 : 0);
    if (used + need + 1 > outLen) break;
    if (n) out[used++] = ',';
    memcpy(out + used, e.name, strlen(e.name));
    used += strlen(e.name);
    out[used] = '\0';
    ++n;
  }
  return n;
}

void Evaluator::reset() {
  level_ = Level::NORMAL;
  upCount_ = downCount_ = 0;
  pendingUp_ = Level::NORMAL;
  histHead_ = histCount_ = 0;
}

void Evaluator::pushTemp(uint32_t tMs, float c) {
  hist_[histHead_] = {tMs, c};
  histHead_ = (histHead_ + 1) % kHistory;
  if (histCount_ < kHistory) ++histCount_;
}

bool Evaluator::riseRate(uint32_t nowMs, float* cPerMin) const {
  if (histCount_ < 2) return false;
  const TempPoint& latest = hist_[(histHead_ + kHistory - 1) % kHistory];
  // Walk from oldest to newest; first point inside the window is the base.
  for (size_t i = 0; i < histCount_; ++i) {
    const TempPoint& p = hist_[(histHead_ + kHistory - histCount_ + i) % kHistory];
    if ((uint32_t)(nowMs - p.tMs) > t_.riseWindowMs) continue;  // too old
    uint32_t span = latest.tMs - p.tMs;                          // wrap-safe
    if (span < t_.riseMinSpanMs) return false;
    *cPerMin = (latest.c - p.c) * 60000.0f / (float)span;
    return true;
  }
  return false;
}

Level Evaluator::classify(const Sample& s, bool riseValid, float rise,
                          float relax, uint16_t* reasons) const {
  // relax = 0 -> nominal thresholds (used to escalate),
  // relax = 1 -> thresholds lowered by hysteresis (used to stay in a level).
  const float gasK = 1.0f - t_.gasHysteresisFrac * relax;
  const float dT = t_.tempHysteresisC * relax;
  const float dR = t_.riseHysteresisCPerMin * relax;

  uint16_t r = REASON_NONE;
  bool gasWarn = false;
  if (s.gasValid) {
    if (s.gasPpm >= t_.gasAlarmPpm * gasK) r |= REASON_GAS_ALARM;
    else if (s.gasPpm >= t_.gasWarnPpm * gasK) r |= REASON_GAS_WARN;
    gasWarn = s.gasPpm >= t_.gasWarnPpm * gasK;
  }
  bool heat = false;
  if (s.tempValid) {
    if (s.tempC >= t_.tempAlarmC - dT) r |= REASON_TEMP_ALARM;
    else if (s.tempC >= t_.tempWarnC - dT) r |= REASON_TEMP_WARN;
    heat = s.tempC >= t_.tempWarnC - dT;
  }
  if (riseValid) {
    if (rise >= t_.riseAlarmCPerMin - dR) r |= REASON_RISE_ALARM;
    else if (rise >= t_.riseWarnCPerMin - dR) r |= REASON_RISE_WARN;
    heat = heat || rise >= t_.riseWarnCPerMin - dR;
  }
  // Smoke/gas together with heat is a strong fire signature.
  if (gasWarn && heat) r |= REASON_COMBINED;

  *reasons = r;
  if (r & (REASON_GAS_ALARM | REASON_TEMP_ALARM | REASON_RISE_ALARM |
           REASON_COMBINED))
    return Level::ALARM;
  if (r) return Level::WARNING;
  return Level::NORMAL;
}

Assessment Evaluator::update(const Sample& s) {
  if (s.tempValid) pushTemp(s.tMs, s.tempC);

  Assessment a;
  a.previous = level_;
  a.riseValid = s.tempValid && riseRate(s.tMs, &a.riseCPerMin);
  if (!a.riseValid) a.riseCPerMin = 0.0f;

  uint16_t rawReasons = 0, holdReasons = 0;
  const Level raw = classify(s, a.riseValid, a.riseCPerMin, 0.0f, &rawReasons);
  const Level hold = classify(s, a.riseValid, a.riseCPerMin, 1.0f, &holdReasons);

  if (raw > level_) {
    // Escalate only after N consecutive samples above the current level; go
    // to the lowest level every sample in the streak agreed on.
    downCount_ = 0;
    pendingUp_ = (upCount_ == 0 || raw < pendingUp_) ? raw : pendingUp_;
    if (++upCount_ >= t_.escalateSamples) {
      level_ = pendingUp_;
      upCount_ = 0;
    }
  } else if (hold < level_) {
    upCount_ = 0;
    if (++downCount_ >= t_.clearSamples) {
      level_ = hold;
      downCount_ = 0;
    }
  } else {
    upCount_ = downCount_ = 0;
  }

  a.level = level_;
  a.rawLevel = raw;
  a.reasons = (level_ > raw) ? holdReasons : rawReasons;
  a.changed = (a.level != a.previous);
  return a;
}

}  // namespace hazard
