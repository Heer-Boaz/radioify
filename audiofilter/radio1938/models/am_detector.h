#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_AM_DETECTOR_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_AM_DETECTOR_H

#include "../../math/biquad.h"

#include <cstdint>
#include <random>

struct Radio1938;

struct AMDetector {
  float fs = 0.0f;
  float bwHz = 0.0f;
  float tuneOffsetHz = 0.0f;

  std::mt19937 rng{0x1942u};
  std::uniform_real_distribution<float> dist{-1.0f, 1.0f};

  Biquad afcErrorLp;

  float audioRect = 0.0f;
  float avcRect = 0.0f;
  float detectorStorageNode = 0.0f;
  float detectorNode = 0.0f;
  float avcEnv = 0.0f;
  float ifWavePhase = 0.0f;
  float prevPrevIfI = 0.0f;
  float prevPrevIfQ = 0.0f;
  float prevIfI = 0.0f;
  float prevIfQ = 0.0f;
  int ifEnvelopeHistorySamples = 0;
  // Conservative detector-island defaults until the multirate numerics are
  // tightened further. With local conduction-aware refinement, 4 spc now
  // tracks the higher-rate reference closely without the extra baseline cost
  // of the earlier 8 spc fallback.
  float waveformSamplesPerCycle = 4.0f;
  int waveformMaxSubsteps = 96;
  int lastWaveformSubsteps = 0;
  uint64_t waveformSampleCount = 0;
  uint64_t waveformIntervalCount = 0;
  uint64_t waveformSplitIntervalCount = 0;
  uint64_t waveformSolveStepCount = 0;
  uint64_t storageSolveCallCount = 0;
  uint64_t storageSolveIterationCount = 0;
  uint32_t storageSolveMaxIterations = 0;
  uint64_t afcProbeTimeNs = 0;
  uint64_t detectorIslandTimeNs = 0;
  uint64_t storageSolveTimeNs = 0;
  bool metricsEnabled = false;
  bool warmStartPending = true;
  float afcError = 0.0f;
  float ifCrackleEnv = 0.0f;
  float ifCrackleDecay = 0.0f;
  float ifCracklePhase = 0.0f;
  uint64_t ifCrackleEventCount = 0;
  float ifCrackleMaxBurstAmp = 0.0f;
  float ifCrackleMaxEnv = 0.0f;

  float audioDiodeDrop = 0.0f;
  float avcDiodeDrop = 0.0f;
  float audioJunctionSlopeVolts = 0.0f;
  float avcJunctionSlopeVolts = 0.0f;
  float detectorStorageCapFarads = 0.0f;
  float audioChargeResistanceOhms = 0.0f;
  float audioDischargeResistanceOhms = 0.0f;
  float avcChargeResistanceOhms = 0.0f;
  float avcDischargeResistanceOhms = 0.0f;
  float avcFilterCapFarads = 0.0f;

  float controlVoltageRef = 0.0f;

  float senseLowHz = 0.0f;
  float senseHighHz = 0.0f;
  float afcSenseLpHz = 0.0f;
  float afcLowOffsetHz = 0.0f;
  float afcHighOffsetHz = 0.0f;
  float afcLowStep = 0.0f;
  float afcHighStep = 0.0f;
  float afcLowPhase = 0.0f;
  float afcHighPhase = 0.0f;
  IQBiquad afcLowProbe;
  IQBiquad afcHighProbe;

  void init(float newFs, float newBw, float newTuneHz = 0.0f);
  void setBandwidth(float newBw, float newTuneHz = 0.0f);
  void setSenseWindow(float lowHz, float highHz);
  void reset();
  float run(float signalI,
            float signalQ,
            float ifNoiseAmp,
            Radio1938& radio,
            float ifCrackleAmp = 0.0f,
            float ifCrackleRate = 0.0f);
};

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_AM_DETECTOR_H
