#ifndef RADIOIFY_AUDIOFILTER_MATH_RADIO_MATH_H
#define RADIOIFY_AUDIOFILTER_MATH_RADIO_MATH_H

#include "../../radio.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>

inline float clampf(float x, float a, float b) {
  return std::min(std::max(x, a), b);
}

template <typename T>
inline T requirePositiveFinite(T x) {
  assert(std::isfinite(x) && x > static_cast<T>(0));
  return x;
}

template <typename T>
inline T requireNonNegativeFinite(T x) {
  assert(std::isfinite(x) && x >= static_cast<T>(0));
  return x;
}

inline float wrapPhase(float phase) {
  while (phase >= kRadioTwoPi) phase -= kRadioTwoPi;
  while (phase < 0.0f) phase += kRadioTwoPi;
  return phase;
}

inline float db2lin(float db) { return std::pow(10.0f, db / 20.0f); }

inline float lin2db(float x) {
  return 20.0f * std::log10(std::max(x, kRadioLinDbFloor));
}

inline float parallelResistance(float a, float b) {
  if (a <= 0.0f) return std::max(b, 0.0f);
  if (b <= 0.0f) return std::max(a, 0.0f);
  return (a * b) / std::max(a + b, 1e-9f);
}

inline float diodeJunctionRectify(float vIn,
                                  float dropVolts,
                                  float junctionSlopeVolts) {
  float slope = requirePositiveFinite(junctionSlopeVolts);
  float x = (vIn - dropVolts) / slope;
  if (x >= 20.0f) return vIn - dropVolts;
  if (x <= -20.0f) return 0.0f;
  return slope * std::log1p(std::exp(x));
}

inline float softplusVolts(float x, float softnessVolts) {
  float slope = requirePositiveFinite(softnessVolts);
  float y = x / slope;
  if (y >= 20.0f) return x;
  if (y <= -20.0f) return 0.0f;
  return slope * std::log1p(std::exp(y));
}

inline double stableLog1pExp(double x) {
  if (x > 50.0) return x;
  if (x < -50.0) return std::exp(x);
  return std::log1p(std::exp(x));
}

inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

inline float seededSignedUnit(uint32_t seed, uint32_t salt) {
  uint32_t h = hash32(seed ^ salt);
  return 2.0f * (static_cast<float>(h) * kRadioHashUnitInv) - 1.0f;
}

inline float applySeededDrift(float base,
                              float relativeDepth,
                              uint32_t seed,
                              uint32_t salt) {
  return base * (1.0f + relativeDepth * seededSignedUnit(seed, salt));
}

template <typename Nonlinear>
inline float processOversampled2x(float x,
                                  float& prev,
                                  Biquad& lp1,
                                  Biquad& lp2,
                                  Nonlinear&& nonlinear) {
  float mid = 0.5f * (prev + x);
  float y0 = nonlinear(mid);
  float y1 = nonlinear(x);
  y0 = lp1.process(y0);
  y0 = lp2.process(y0);
  y1 = lp1.process(y1);
  y1 = lp2.process(y1);
  prev = x;
  return y1;
}

inline float softClip(float x, float t = kRadioSoftClipThresholdDefault) {
  float ax = std::fabs(x);
  if (ax <= t) return x;
  float s = (x < 0.0f) ? -1.0f : 1.0f;
  float u = (ax - t) / (1.0f - t);
  float y = t + (1.0f - std::exp(-u)) * (1.0f - t);
  return s * y;
}

#endif
