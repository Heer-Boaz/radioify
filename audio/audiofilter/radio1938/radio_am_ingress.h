#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_AM_INGRESS_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_AM_INGRESS_H

#include "radio_reception_profile.h"

#include <cstdint>

struct RadioAmReceptionSample {
  // Complex gain of the direct (groundwave) path of the desired AM waveform.
  float desiredCarrierI = 1.0f;
  float desiredCarrierQ = 0.0f;
  // Complex gain of a second, later-arriving copy of the same waveform
  // (skywave). Its RF phase is part of the gain; the delay applies to the
  // modulation, so sidebands and carrier fade differently.
  float skywaveI = 0.0f;
  float skywaveQ = 0.0f;
  float skywaveDelaySamples = 0.0f;
  // Optional already-real RF voltage from another carrier.
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

// The received carrier level follows the reception field and antenna
// (radioReceptionAntennaCarrierRmsVolts).
struct RadioAmIngressConfig {
  float modulationIndex = 0.85f;
  RadioBroadcastSourceConfig broadcastSource;
  RadioReceptionConfig reception;
};

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_AM_INGRESS_H
