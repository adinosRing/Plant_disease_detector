// MQ2 conversion maths (header-only, no Arduino dependencies).
//
// The MQ2 is a heated SnO2 resistor whose resistance Rs drops as the
// concentration of combustible gas / smoke rises. The module wires it in
// series with a load resistor RL; the analog output is the voltage across RL:
//
//     Vout = Vc * RL / (Rs + RL)   =>   Rs = RL * (Vc - Vout) / Vout
//
// Concentration is estimated from the ratio Rs/R0, where R0 is Rs in clean
// air. In clean air the datasheet gives Rs/R0 ~= 9.83. The smoke curve on the
// datasheet's log-log chart is approximated by a straight line through
// (log10(200 ppm), log10(3.4)) with slope -0.44.
//
// These numbers are approximations: treat the ppm value as a *smoke-
// equivalent index* for thresholding, not as a calibrated measurement.
#pragma once

#include <math.h>

namespace mq2 {

constexpr float kCleanAirRatio = 9.83f;

// Smoke curve: {log10(ppm) at reference point, log10(ratio) there, slope}.
struct Curve { float x; float y; float slope; };
constexpr Curve kSmokeCurve = {2.30f, 0.53f, -0.44f};
constexpr Curve kLpgCurve   = {2.30f, 0.21f, -0.47f};

// Returns Rs in kOhm, or a negative value if the reading is unusable.
inline float sensorResistanceKOhm(float vOut, float vCircuit, float rLoadKOhm) {
  if (vOut <= 0.01f || vOut >= vCircuit) return -1.0f;
  return rLoadKOhm * (vCircuit - vOut) / vOut;
}

inline float r0FromCleanAirRs(float rsCleanAir) {
  return rsCleanAir / kCleanAirRatio;
}

inline float ppmFromRatio(float ratio, const Curve& c = kSmokeCurve) {
  if (ratio <= 0.0f) return -1.0f;
  return powf(10.0f, ((log10f(ratio) - c.y) / c.slope) + c.x);
}

}  // namespace mq2
