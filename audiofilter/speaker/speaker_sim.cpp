#include "../../radio.h"
#include "../math/radio_math.h"

#include <algorithm>
#include <cmath>

void SpeakerSim::init(float fs) {
  float suspensionHzDerived =
      suspensionHz * (1.0f + 0.45f * coneMassTolerance -
                      0.65f * suspensionComplianceTolerance);
  float coneBodyHzDerived =
      coneBodyHz * (1.0f + 0.22f * coneMassTolerance +
                    0.16f * voiceCoilTolerance);
  suspensionRes.setPeaking(fs, suspensionHzDerived, suspensionQ, suspensionGainDb);
  coneBody.setPeaking(fs, coneBodyHzDerived, coneBodyQ, coneBodyGainDb);
  upperBreakup = Biquad{};
  coneDip = Biquad{};
  if (topLpHz > 0.0f) {
    float topLpHzDerived = topLpHz / (1.0f + 0.40f * voiceCoilTolerance);
    topLp.setLowpass(fs, topLpHzDerived, filterQ);
  } else {
    topLp = Biquad{};
  }
  hfLossLp = Biquad{};
  excursionAtk = std::exp(-1.0f / (fs * 0.010f));
  excursionRel = std::exp(-1.0f / (fs * 0.120f));
}

void SpeakerSim::reset() {
  suspensionRes.reset();
  coneBody.reset();
  upperBreakup.reset();
  coneDip.reset();
  topLp.reset();
  hfLossLp.reset();
  excursionEnv = 0.0f;
}

float SpeakerSim::process(float x, bool& clipped) {
  float y = x * std::max(drive, 0.0f);
  y = suspensionRes.process(y);
  y = coneBody.process(y);
  if (topLpHz > 0.0f) {
    y = topLp.process(y);
  }

  float a = std::fabs(y);
  if (a > excursionEnv) {
    excursionEnv = excursionAtk * excursionEnv + (1.0f - excursionAtk) * a;
  } else {
    excursionEnv = excursionRel * excursionEnv + (1.0f - excursionRel) * a;
  }

  float excursionT =
      clampf(excursionEnv / std::max(excursionRef, 1e-6f), 0.0f, 1.0f);
  float complianceGain = 1.0f - complianceLossDepth * excursionT;
  y *= std::max(0.70f, complianceGain);
  clipped = limit > 0.0f && std::fabs(y) > limit;
  if (limit > 0.0f && limit < 1.0f) {
    return softClip(y, limit);
  }
  return y;
}
