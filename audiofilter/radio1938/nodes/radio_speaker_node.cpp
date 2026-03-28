#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

void initSpeakerModel(SpeakerSim& speaker, float fs) {
  float suspensionHzDerived =
      speaker.suspensionHz * (1.0f + 0.45f * speaker.coneMassTolerance -
                              0.65f * speaker.suspensionComplianceTolerance);
  float coneBodyHzDerived =
      speaker.coneBodyHz * (1.0f + 0.22f * speaker.coneMassTolerance +
                            0.16f * speaker.voiceCoilTolerance);
  speaker.suspensionRes.setPeaking(fs, suspensionHzDerived, speaker.suspensionQ,
                                   speaker.suspensionGainDb);
  speaker.coneBody.setPeaking(fs, coneBodyHzDerived, speaker.coneBodyQ,
                              speaker.coneBodyGainDb);
  speaker.upperBreakup = Biquad{};
  speaker.coneDip = Biquad{};
  if (speaker.topLpHz > 0.0f) {
    float topLpHzDerived =
        speaker.topLpHz / (1.0f + 0.40f * speaker.voiceCoilTolerance);
    speaker.topLp.setLowpass(fs, topLpHzDerived, speaker.filterQ);
  } else {
    speaker.topLp = Biquad{};
  }
  speaker.hfLossLp = Biquad{};
  speaker.excursionAtk = std::exp(-1.0f / (fs * 0.010f));
  speaker.excursionRel = std::exp(-1.0f / (fs * 0.120f));
}

void resetSpeakerModel(SpeakerSim& speaker) {
  speaker.suspensionRes.reset();
  speaker.coneBody.reset();
  speaker.upperBreakup.reset();
  speaker.coneDip.reset();
  speaker.topLp.reset();
  speaker.hfLossLp.reset();
  speaker.excursionEnv = 0.0f;
}

float runSpeakerModel(SpeakerSim& speaker, float x, bool& clipped) {
  float y = x * std::max(speaker.drive, 0.0f);
  y = speaker.suspensionRes.process(y);
  y = speaker.coneBody.process(y);
  if (speaker.topLpHz > 0.0f) {
    y = speaker.topLp.process(y);
  }

  float a = std::fabs(y);
  if (a > speaker.excursionEnv) {
    speaker.excursionEnv =
        speaker.excursionAtk * speaker.excursionEnv +
        (1.0f - speaker.excursionAtk) * a;
  } else {
    speaker.excursionEnv =
        speaker.excursionRel * speaker.excursionEnv +
        (1.0f - speaker.excursionRel) * a;
  }

  float excursionT =
      clampf(speaker.excursionEnv / std::max(speaker.excursionRef, 1e-6f),
             0.0f, 1.0f);
  float complianceGain = 1.0f - speaker.complianceLossDepth * excursionT;
  y *= std::max(0.70f, complianceGain);
  clipped = speaker.limit > 0.0f && std::fabs(y) > speaker.limit;
  if (speaker.limit > 0.0f && speaker.limit < 1.0f) {
    return softClip(y, speaker.limit);
  }
  return y;
}

}  // namespace

void RadioSpeakerNode::init(Radio1938& radio, RadioInitContext&) {
  auto& speakerStage = radio.speakerStage;
  float osFs = radio.sampleRate * radio.globals.oversampleFactor;
  float osCut = radio.sampleRate * radio.globals.oversampleCutoffFraction;
  speakerStage.osLpIn.setLowpass(osFs, osCut, kRadioBiquadQ);
  speakerStage.osLpOut.setLowpass(osFs, osCut, kRadioBiquadQ);
  initSpeakerModel(speakerStage.speaker, osFs);
  speakerStage.speaker.drive = speakerStage.drive;
}

void RadioSpeakerNode::reset(Radio1938& radio) {
  auto& speakerStage = radio.speakerStage;
  speakerStage.osPrev = 0.0f;
  speakerStage.osLpIn.reset();
  speakerStage.osLpOut.reset();
  resetSpeakerModel(speakerStage.speaker);
}

float RadioSpeakerNode::run(Radio1938& radio, float y, RadioSampleContext&) {
  auto& speakerStage = radio.speakerStage;
  speakerStage.speaker.drive = std::max(speakerStage.drive, 0.0f);
  y = processOversampled2x(y, speakerStage.osPrev, speakerStage.osLpIn,
                           speakerStage.osLpOut, [&](float v) {
                             bool clipped = false;
                             float out =
                                 runSpeakerModel(speakerStage.speaker, v, clipped);
                             if (clipped) radio.diagnostics.markSpeakerClip();
                             return out;
                           });
  return y;
}
