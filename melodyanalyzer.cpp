#include "melodyanalyzer.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr int kMelodyAnalysisHop = 256;
constexpr float kMelodyMinFrequencyHz = 55.0f;
constexpr float kMelodyMaxFrequencyHz = 1200.0f;
constexpr float kMelodyConfidenceFloor = 0.16f;
constexpr float kMelodyConfidenceHold = 0.08f;
constexpr float kMelodySmoothing = 0.86f;
constexpr float kMelodyJumpRejectSemitones = 7.5f;
constexpr float kMelodyJumpRejectConfidence = 0.34f;
constexpr float kMelodyConfidenceDecay = 0.82f;

int midiFromFrequency(float frequencyHz) {
  if (!std::isfinite(frequencyHz) || frequencyHz <= 0.0f) return -1;
  float midi = 69.0f + 12.0f * std::log2(frequencyHz / 440.0f);
  if (!std::isfinite(midi)) return -1;
  int rounded = static_cast<int>(std::lround(midi));
  if (rounded < 0 || rounded > 127) return -1;
  return rounded;
}

float computeLagCorrelation(const float* samples, int sampleCount, int lag) {
  if (!samples || lag <= 0 || lag >= sampleCount) return 0.0f;
  double sumXY = 0.0;
  double sumXX = 0.0;
  double sumYY = 0.0;
  int limit = sampleCount - lag;
  for (int i = 0; i < limit; ++i) {
    double x = static_cast<double>(samples[i]);
    double y = static_cast<double>(samples[i + lag]);
    sumXY += x * y;
    sumXX += x * x;
    sumYY += y * y;
  }
  if (sumXX <= 0.0 || sumYY <= 0.0) return 0.0f;
  return static_cast<float>(sumXY / std::sqrt(sumXX * sumYY));
}

void decayConfidenceOnly(MelodyAnalyzerState* state) {
  if (!state) return;
  float held = state->confidence.load(std::memory_order_relaxed);
  if (held > kMelodyConfidenceHold) {
    held *= kMelodyConfidenceDecay;
    if (held < kMelodyConfidenceHold) held = 0.0f;
    state->confidence.store(held, std::memory_order_relaxed);
  }
}

void applyAnalysis(MelodyAnalyzerState* state,
                   const float* window,
                   int windowSize,
                   uint32_t sampleRate) {
  if (!state || !window || windowSize < 3 || sampleRate == 0) return;

  int minLag = static_cast<int>(
      std::floor(static_cast<float>(sampleRate) / kMelodyMaxFrequencyHz));
  int maxLag = static_cast<int>(
      std::floor(static_cast<float>(sampleRate) / kMelodyMinFrequencyHz));
  minLag = std::max(1, minLag);
  maxLag = std::max(minLag + 1, std::min(windowSize - 1, maxLag));
  if (minLag < 1 || maxLag <= minLag) return;

  double mean = 0.0;
  for (int i = 0; i < windowSize; ++i) {
    mean += static_cast<double>(window[i]);
  }
  mean /= static_cast<double>(windowSize);

  std::array<float, kMelodyAnalyzerWindowFrames> centered{};
  for (int i = 0; i < windowSize; ++i) {
    centered[static_cast<size_t>(i)] = window[i] - static_cast<float>(mean);
  }

  double energy = 0.0;
  for (int i = 0; i < windowSize; ++i) {
    energy += static_cast<double>(centered[static_cast<size_t>(i)] *
                                  centered[static_cast<size_t>(i)]);
  }
  if (energy < 1e-8) {
    float held = state->confidence.load(std::memory_order_relaxed);
    if (held > kMelodyConfidenceHold) {
      held *= kMelodyConfidenceDecay;
      if (held < kMelodyConfidenceHold) held = 0.0f;
      state->confidence.store(held, std::memory_order_relaxed);
      if (held <= 0.0f) {
        state->frequencyHz.store(0.0f, std::memory_order_relaxed);
        state->midiNote.store(-1, std::memory_order_relaxed);
        state->smoothedFrequencyHz = 0.0f;
      }
    } else {
      melodyAnalyzerReset(state);
    }
    return;
  }

  int bestLag = -1;
  float bestCorrelation = 0.0f;
  for (int lag = minLag; lag <= maxLag; ++lag) {
    float correlation = computeLagCorrelation(centered.data(), windowSize, lag);
    if (correlation > bestCorrelation) {
      bestCorrelation = correlation;
      bestLag = lag;
    }
  }

  if (bestLag <= 0 || bestCorrelation < kMelodyConfidenceFloor) {
    float held = state->confidence.load(std::memory_order_relaxed);
    if (held > kMelodyConfidenceHold) {
      held *= kMelodyConfidenceDecay;
      if (held < kMelodyConfidenceHold) held = 0.0f;
      state->confidence.store(held, std::memory_order_relaxed);
    } else {
      state->frequencyHz.store(0.0f, std::memory_order_relaxed);
      state->confidence.store(0.0f, std::memory_order_relaxed);
      state->midiNote.store(-1, std::memory_order_relaxed);
      state->smoothedFrequencyHz = 0.0f;
    }
    return;
  }

  float left = computeLagCorrelation(centered.data(), windowSize,
                                     std::max(minLag, bestLag - 1));
  float center = bestCorrelation;
  float right = computeLagCorrelation(centered.data(), windowSize,
                                      std::min(maxLag, bestLag + 1));
  float lagOffset = 0.0f;
  float denominator = (left - 2.0f * center + right);
  if (std::fabs(denominator) > 1e-6f) {
    lagOffset = 0.5f * (left - right) / denominator;
    lagOffset = std::clamp(lagOffset, -0.5f, 0.5f);
  }
  float refinedLag = static_cast<float>(bestLag) + lagOffset;
  if (refinedLag <= 0.0f) refinedLag = static_cast<float>(bestLag);
  float freq = static_cast<float>(sampleRate) / refinedLag;
  if (!std::isfinite(freq) || freq < kMelodyMinFrequencyHz ||
      freq > kMelodyMaxFrequencyHz) {
    return;
  }

  if (state->smoothedFrequencyHz > 0.0f) {
    float semitoneJump = std::fabs(
        12.0f *
        std::log2(freq / std::max(1e-6f, state->smoothedFrequencyHz)));
    if (semitoneJump > kMelodyJumpRejectSemitones &&
        bestCorrelation < kMelodyJumpRejectConfidence) {
      decayConfidenceOnly(state);
      return;
    }
  }

  if (state->smoothedFrequencyHz <= 0.0f) {
    state->smoothedFrequencyHz = freq;
  } else {
    state->smoothedFrequencyHz = (state->smoothedFrequencyHz * kMelodySmoothing) +
                                 (freq * (1.0f - kMelodySmoothing));
  }

  float prevConfidence = state->confidence.load(std::memory_order_relaxed);
  float smoothedConfidence =
      std::clamp((prevConfidence * 0.65f) + (bestCorrelation * 0.35f), 0.0f,
                 1.0f);
  if (smoothedConfidence < kMelodyConfidenceFloor) {
    if (prevConfidence > kMelodyConfidenceHold) {
      smoothedConfidence = prevConfidence * kMelodyConfidenceDecay;
      if (smoothedConfidence < kMelodyConfidenceHold) {
        smoothedConfidence = 0.0f;
      }
    } else {
      smoothedConfidence = 0.0f;
    }
  }

  state->frequencyHz.store(state->smoothedFrequencyHz, std::memory_order_relaxed);
  state->confidence.store(smoothedConfidence, std::memory_order_relaxed);
  state->midiNote.store(midiFromFrequency(state->smoothedFrequencyHz),
                        std::memory_order_relaxed);
}
}  // namespace

void melodyAnalyzerReset(MelodyAnalyzerState* state) {
  if (!state) return;
  state->window.fill(0.0f);
  state->windowPos = 0;
  state->windowCount = 0;
  state->samplesSinceAnalysis = 0;
  state->smoothedFrequencyHz = 0.0f;
  state->frequencyHz.store(0.0f, std::memory_order_relaxed);
  state->confidence.store(0.0f, std::memory_order_relaxed);
  state->midiNote.store(-1, std::memory_order_relaxed);
}

void melodyAnalyzerUpdate(MelodyAnalyzerState* state,
                          const float* samples,
                          uint32_t frameCount,
                          uint32_t channels,
                          uint32_t sampleRate) {
  if (!state || !samples || frameCount == 0 || channels == 0 || sampleRate == 0) {
    return;
  }

  for (uint32_t i = 0; i < frameCount; ++i) {
    float mono = samples[static_cast<size_t>(i) * channels];
    if (channels > 1) {
      for (uint32_t ch = 1; ch < channels; ++ch) {
        mono += samples[static_cast<size_t>(i) * channels + ch];
      }
      mono /= static_cast<float>(channels);
    }
    state->window[state->windowPos] = mono;
    state->windowPos = (state->windowPos + 1) % kMelodyAnalyzerWindowFrames;
    if (state->windowCount < kMelodyAnalyzerWindowFrames) {
      ++state->windowCount;
    }
    ++state->samplesSinceAnalysis;
  }

  if (state->windowCount < kMelodyAnalyzerWindowFrames ||
      state->samplesSinceAnalysis < kMelodyAnalysisHop) {
    return;
  }
  state->samplesSinceAnalysis = 0;

  int windowSize = std::min(kMelodyAnalyzerWindowFrames, state->windowCount);
  int readStart = state->windowPos - windowSize;
  if (readStart < 0) readStart += kMelodyAnalyzerWindowFrames;

  std::array<float, kMelodyAnalyzerWindowFrames> analysisWindow{};
  for (int i = 0; i < windowSize; ++i) {
    analysisWindow[static_cast<size_t>(i)] =
        state->window[(readStart + i) % kMelodyAnalyzerWindowFrames];
  }
  applyAnalysis(state, analysisWindow.data(), windowSize, sampleRate);
}

MelodyAnalyzerInfo melodyAnalyzerGetInfo(const MelodyAnalyzerState* state) {
  MelodyAnalyzerInfo info;
  if (!state) return info;
  info.frequencyHz = state->frequencyHz.load(std::memory_order_relaxed);
  info.confidence = state->confidence.load(std::memory_order_relaxed);
  info.midiNote = state->midiNote.load(std::memory_order_relaxed);
  return info;
}

