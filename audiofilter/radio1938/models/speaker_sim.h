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
};

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_SPEAKER_SIM_H
