#pragma once

#include <array>
#include <atomic>
#include <cstdint>

constexpr int kMelodyAnalyzerWindowFrames = 2048;

struct MelodyAnalyzerState {
  std::array<float, kMelodyAnalyzerWindowFrames> window{};
  int windowPos = 0;
  int windowCount = 0;
  int samplesSinceAnalysis = 0;
  std::atomic<float> frequencyHz{0.0f};
  std::atomic<float> confidence{0.0f};
  std::atomic<int> midiNote{-1};
  float smoothedFrequencyHz = 0.0f;
};

struct MelodyAnalyzerInfo {
  float frequencyHz = 0.0f;
  float confidence = 0.0f;
  int midiNote = -1;
};

void melodyAnalyzerReset(MelodyAnalyzerState* state);
void melodyAnalyzerUpdate(MelodyAnalyzerState* state,
                          const float* samples,
                          uint32_t frameCount,
                          uint32_t channels,
                          uint32_t sampleRate);
MelodyAnalyzerInfo melodyAnalyzerGetInfo(const MelodyAnalyzerState* state);

