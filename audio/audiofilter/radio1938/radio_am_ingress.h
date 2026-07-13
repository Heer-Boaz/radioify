#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_AM_INGRESS_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_AM_INGRESS_H

#include "radio_reception_profile.h"

#include <cstdint>

struct RadioAmReceptionSample {
  // Complex multiplier for the complete desired AM waveform, followed by an
  // optional already-real RF voltage from another carrier.
  float desiredCarrierI = 1.0f;
  float desiredCarrierQ = 0.0f;
  float additiveRf = 0.0f;
};

struct RadioBroadcastSourceConfig {
  bool enabled = true;

  bool compressionEnabled = true;
  float compressionThresholdModulation = 0.80f;
  float compressionRatio = 2.5f;
  float compressionAttackMs = 20.0f;
  float compressionReleaseMs = 250.0f;

  bool nonlinearityEnabled = true;
  float cubicNonlinearity = 0.04f;

  bool noiseEnabled = true;
  float noiseBelowFullModulationDb = 64.0f;
  uint32_t noiseSeed = 0x1938110au;
};

struct RadioAmIngressConfig {
  float receivedCarrierRmsVolts = 0.12f;
  float modulationIndex = 0.85f;
  RadioBroadcastSourceConfig broadcastSource;
  RadioReceptionConfig reception;
};

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_AM_INGRESS_H
