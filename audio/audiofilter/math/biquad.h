#ifndef RADIOIFY_AUDIOFILTER_MATH_BIQUAD_H
#define RADIOIFY_AUDIOFILTER_MATH_BIQUAD_H

#include <array>
#include <cmath>

struct Biquad {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
  float z1 = 0.0f;
  float z2 = 0.0f;

  float process(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }

  void reset() {
    z1 = 0.0f;
    z2 = 0.0f;
  }

  void setLowpass(float sampleRate, float freq, float q) {
    constexpr float kTwoPi = 6.28318530717958647692f;
    float w0 = kTwoPi * freq / sampleRate;
    float cosw = std::cos(w0);
    float sinw = std::sin(w0);
    float alpha = sinw / (2.0f * q);
    float a0 = 1.0f + alpha;
    b0 = (1.0f - cosw) / 2.0f / a0;
    b1 = (1.0f - cosw) / a0;
    b2 = (1.0f - cosw) / 2.0f / a0;
    a1 = -2.0f * cosw / a0;
    a2 = (1.0f - alpha) / a0;
  }

  void setHighpass(float sampleRate, float freq, float q) {
    constexpr float kTwoPi = 6.28318530717958647692f;
    float w0 = kTwoPi * freq / sampleRate;
    float cosw = std::cos(w0);
    float sinw = std::sin(w0);
    float alpha = sinw / (2.0f * q);
    float a0 = 1.0f + alpha;
    b0 = (1.0f + cosw) / 2.0f / a0;
    b1 = -(1.0f + cosw) / a0;
    b2 = (1.0f + cosw) / 2.0f / a0;
    a1 = -2.0f * cosw / a0;
    a2 = (1.0f - alpha) / a0;
  }

  void setBandpass(float sampleRate, float freq, float q) {
    constexpr float kTwoPi = 6.28318530717958647692f;
    float w0 = kTwoPi * freq / sampleRate;
    float cosw = std::cos(w0);
    float sinw = std::sin(w0);
    float alpha = sinw / (2.0f * q);
    float a0 = 1.0f + alpha;
    b0 = alpha / a0;
    b1 = 0.0f;
    b2 = -alpha / a0;
    a1 = -2.0f * cosw / a0;
    a2 = (1.0f - alpha) / a0;
  }

  void setPeaking(float sampleRate, float freq, float q, float gainDb) {
    constexpr float kTwoPi = 6.28318530717958647692f;
    float a = std::pow(10.0f, gainDb / 40.0f);
    float w0 = kTwoPi * freq / sampleRate;
    float cosw = std::cos(w0);
    float sinw = std::sin(w0);
    float alpha = sinw / (2.0f * q);
    float a0 = 1.0f + alpha / a;
    b0 = (1.0f + alpha * a) / a0;
    b1 = -2.0f * cosw / a0;
    b2 = (1.0f - alpha * a) / a0;
    a1 = -2.0f * cosw / a0;
    a2 = (1.0f - alpha / a) / a0;
  }
};

struct IQBiquad {
  Biquad i;
  Biquad q;

  void reset() {
    i.reset();
    q.reset();
  }

  void setLowpass(float sampleRate, float freq, float qValue) {
    i.setLowpass(sampleRate, freq, qValue);
    q.setLowpass(sampleRate, freq, qValue);
  }

  std::array<float, 2> process(float inI, float inQ) {
    return {i.process(inI), q.process(inQ)};
  }
};

#endif  // RADIOIFY_AUDIOFILTER_MATH_BIQUAD_H
