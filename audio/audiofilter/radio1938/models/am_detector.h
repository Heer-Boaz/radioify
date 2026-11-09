#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_AM_DETECTOR_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_AM_DETECTOR_H

#include "../../math/biquad.h"
#include "../../models/thermionic_diode.h"

#include <cstdint>
#include <random>

struct Radio1938;

struct AMDetector {
  float fs = 0.0f;
  float bwHz = 0.0f;
  float tuneOffsetHz = 0.0f;

  Biquad afcErrorLp;

  float audioRect = 0.0f;
  float detectorStorageNode = 0.0f;
  float filteredStorageNode = 0.0f;
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
  ThermionicDiode audioDiode;
  // AVC line: an RC filter whose source is either the DC of the detector load
  // (simple AVC, the audio node) or a separate delayed rectifier on the IF
  // envelope. The controlled grid returns draw no DC from it. The delay bias
  // is subtracted from the source before filtering and requires the separate
  // rectifier. Simple AVC loads the detector through its filter resistor.
  bool avcFromIfEnvelope = false;
  // The separate rectifier takes the last IF plate (primary), which the IF
  // strip relates to the detector input at the centre frequency.
  float avcRectifierPerIfVolt = 1.0f;
  float avcDelayBiasVolts = 0.0f;
  float avcFilterResistanceOhms = 0.0f;
  float avcTick = 0.0f;
  float detectorStorageCapFarads = 0.0f;
  float filterResistanceOhms = 0;
  float filterCapFarads = 0;
  float audioChargeResistanceOhms = 0.0f;
  float audioDischargeResistanceOhms = 0.0f;
  float avcFilterCapFarads = 0.0f;

  float controlVoltageRef = 0.0f;

  float senseLowHz = 0.0f;
  float senseHighHz = 0.0f;
  float afcSenseLpHz = 0.0f;
  float afcLowOffsetHz = 0.0f;
  float afcHighOffsetHz = 0.0f;
  float afcLowStep = 0.0f;
  float afcHighStep = 0.0f;
  float afcLowStepCos = 1.0f;
  float afcLowStepSin = 0.0f;
  float afcHighStepCos = 1.0f;
  float afcHighStepSin = 0.0f;
  float afcLowPhase = 0.0f;
  float afcHighPhase = 0.0f;
  float afcLowOscCos = 1.0f;
  float afcLowOscSin = 0.0f;
  float afcHighOscCos = 1.0f;
  float afcHighOscSin = 0.0f;
  IQBiquad afcLowProbe;
  IQBiquad afcHighProbe;

  void init(float newFs, float newBw, float newTuneHz = 0.0f);
  void setBandwidth(float newBw, float newTuneHz = 0.0f);
  void setSenseWindow(float lowHz, float highHz);
  void reset();
  float run(float signalI, float signalQ, Radio1938& radio);
};

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_AM_DETECTOR_H
