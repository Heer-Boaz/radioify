#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_SPEAKER_SIM_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_SPEAKER_SIM_H

#include "../../math/biquad.h"

struct SpeakerSim {
  Biquad suspensionRes;
  Biquad coneBody;
  Biquad upperBreakup;
  Biquad coneDip;
  Biquad topLp;
  Biquad hfLossLp;
  float electricalSampleRate = 0.0f;
  float drive = 0.0f;
  float limit = 0.0f;
  float asymBias = 0.0f;
  float filterQ = 0.0f;
  float suspensionHz = 0.0f;
  float suspensionQ = 0.0f;
  float suspensionGainDb = 0.0f;
  float coneBodyHz = 0.0f;
  float coneBodyQ = 0.0f;
  float coneBodyGainDb = 0.0f;
  float upperBreakupHz = 0.0f;
  float upperBreakupQ = 0.0f;
  float upperBreakupGainDb = 0.0f;
  float coneDipHz = 0.0f;
  float coneDipQ = 0.0f;
  float coneDipGainDb = 0.0f;
  float topLpHz = 0.0f;
  float hfLossLpHz = 0.0f;
  float suspensionComplianceTolerance = 0.0f;
  float coneMassTolerance = 0.0f;
  float breakupTolerance = 0.0f;
  float voiceCoilTolerance = 0.0f;
  float excursionEnv = 0.0f;
  float excursionAtk = 0.0f;
  float excursionRel = 0.0f;
  float excursionRef = 0.0f;
  float complianceLossDepth = 0.0f;
  float hfLossDepth = 0.0f;
  float voiceCoilResistanceOhms = 0.0f;
  float voiceCoilInductanceHenries = 0.0f;
  float movingMassKg = 0.0f;
  float mechanicalQ = 0.0f;
  float electricalQ = 0.0f;
  float forceFactorBl = 0.0f;
  float suspensionComplianceMetersPerNewton = 0.0f;
  float mechanicalDampingNsPerMeter = 0.0f;
  float nominalLoadOhms = 0.0f;
  float electricalCurrentAmps = 0.0f;
  float coneVelocityMetersPerSecond = 0.0f;
  float coneDisplacementMeters = 0.0f;
  float backEmfVolts = 0.0f;
  float loadSenseVoltage = 0.0f;
  float loadSenseCurrent = 0.0f;
  float loadSenseCoeff = 0.0f;
  float effectiveLoadOhms = 0.0f;
};

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_SPEAKER_SIM_H
