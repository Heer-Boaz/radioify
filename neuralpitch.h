#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "melodyanalyzer.h"

struct NeuralPitchFrame {
  uint64_t sourceFrame = 0;
  float frequencyHz = 0.0f;
  float voicingProb = 0.0f;
  int midiNote = -1;
  std::array<float, kMelodyAnalyzerStates> posterior{};
};

struct NeuralPitchState {
  bool active = false;
  bool hasLatest = false;
  uint32_t sourceSampleRate = 0;
  uint32_t channels = 0;

  double sourcePos = 0.0;
  double sourceStep = 0.0;
  uint64_t sourceFramesProcessed = 0;

  std::vector<float> sourceBuffer;
  std::deque<float> ring16k;
  int samplesSinceHop = 0;

  NeuralPitchFrame latest{};

  struct RuntimeOpaque;
  RuntimeOpaque* runtime = nullptr;
};

void neuralPitchReset(NeuralPitchState* state);
bool neuralPitchInit(NeuralPitchState* state, uint32_t sourceSampleRate,
                     uint32_t channels, std::string* error);
void neuralPitchUninit(NeuralPitchState* state);
void neuralPitchUpdate(NeuralPitchState* state, const float* samples,
                       uint32_t frameCount, uint32_t channels);
bool neuralPitchIsActive(const NeuralPitchState* state);
bool neuralPitchHasFrame(const NeuralPitchState* state);
NeuralPitchFrame neuralPitchGetLatest(const NeuralPitchState* state);
