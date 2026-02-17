#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>

constexpr int kMelodyAnalyzerWindowFrames = 2048;
constexpr int kMelodyMidiMin = 21;
constexpr int kMelodyMidiMax = 108;
constexpr int kMelodyAnalyzerStates =
    (kMelodyMidiMax - kMelodyMidiMin + 2);  // +1 unvoiced state
constexpr int kMelodyTempoMinBpm = 48;
constexpr int kMelodyTempoMaxBpm = 220;
constexpr int kMelodyTempoBinCount = (kMelodyTempoMaxBpm - kMelodyTempoMinBpm + 1);
constexpr int kMelodyUnvoicedState = 0;

struct MelodyResamplerState {
  uint32_t srcRate = 0;
  uint32_t dstRate = 0;
  double sourcePos = 0.0;
  double sourceStep = 0.0;
  std::deque<float> inputSamples;
};

struct MelodyAnalyzerState {
  MelodyResamplerState pitchResampler{};
  MelodyResamplerState hpcpResampler{};
  std::deque<float> pitchRing{};
  std::deque<float> hpcpRing{};
  int pitchSamplesSinceAnalysis = 0;
  int hpcpSamplesSinceAnalysis = 0;
  std::array<float, kMelodyAnalyzerStates> viterbiPrev{};
  std::array<float, kMelodyAnalyzerStates> viterbiCur{};
  std::array<int, kMelodyAnalyzerStates> viterbiBack{};
  std::array<float, kMelodyAnalyzerStates> lastPosterior{};
  std::deque<float> hpcpOnsetHistory{};
  std::array<float, 12> hpcpProfile{};
  std::array<float, 12> prevHpcpProfile{};
  float dcPrevInput = 0.0f;
  float dcPrevOutput = 0.0f;
  float dcAlpha = 0.999f;
  float prevHpcpFrameEnergy = 0.0f;
  float tuningCents = 0.0f;
  float frameEnergy = 0.0f;
  float hpcpEntropy = 0.0f;
  float voicingStrength = 0.0f;
  float tempoBpm = 0.0f;
  float beatConfidence = 0.0f;
  bool viterbiInited = false;
  uint32_t sampleRate = 0;
  std::atomic<float> frequencyHz{0.0f};
  std::atomic<float> confidence{0.0f};
  std::atomic<int> midiNote{-1};
  float lastPitchHz = 0.0f;
};

struct MelodyAnalyzerInfo {
  float frequencyHz = 0.0f;
  float confidence = 0.0f;
  int midiNote = -1;
  float harmonicity = 0.0f;
  float tempoBpm = 0.0f;
  float beatConfidence = 0.0f;
  std::array<float, 12> hpcpProfile{};
};

void melodyAnalyzerReset(MelodyAnalyzerState* state);
void melodyAnalyzerUpdate(MelodyAnalyzerState* state, const float* samples,
                         uint32_t frameCount, uint32_t channels,
                         uint32_t sampleRate);
MelodyAnalyzerInfo melodyAnalyzerGetInfo(const MelodyAnalyzerState* state);
