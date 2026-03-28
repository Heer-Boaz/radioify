#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

float clampSpeakerFilterHz(float fs, float hz) {
  return std::clamp(hz, 20.0f, 0.45f * std::max(fs, 1.0f));
}

float deriveSpeakerCompliance(float movingMassKg, float suspensionHz) {
  float w0 = kRadioTwoPi * std::max(suspensionHz, 1.0f);
  return 1.0f / std::max(movingMassKg * w0 * w0, 1e-9f);
}

float deriveSpeakerMechanicalDamping(float movingMassKg,
                                     float suspensionHz,
                                     float suspensionQ) {
  float q = std::max(suspensionQ, 0.35f);
  float w0 = kRadioTwoPi * std::max(suspensionHz, 1.0f);
  return (movingMassKg * w0) / q;
}

float deriveSpeakerVoiceCoilInductance(float voiceCoilResistanceOhms,
                                       float topLpHz) {
  float lpHz = std::max(topLpHz, 2500.0f);
  return voiceCoilResistanceOhms / (kRadioTwoPi * lpHz);
}

float deriveSpeakerForceFactor(float movingMassKg,
                               float suspensionHz,
                               float voiceCoilResistanceOhms) {
  constexpr float kElectricalQ = 0.95f;
  float w0 = kRadioTwoPi * std::max(suspensionHz, 1.0f);
  return std::sqrt(std::max(w0 * movingMassKg * voiceCoilResistanceOhms /
                                kElectricalQ,
                            1e-9f));
}

float deriveSpeakerMechanicalDampingFromQ(float movingMassKg,
                                          float suspensionHz,
                                          float mechanicalQ) {
  float q = std::max(mechanicalQ, 0.35f);
  float w0 = kRadioTwoPi * std::max(suspensionHz, 1.0f);
  return (movingMassKg * w0) / q;
}

float deriveSpeakerForceFactorFromQ(float movingMassKg,
                                    float suspensionHz,
                                    float voiceCoilResistanceOhms,
                                    float electricalQ) {
  float q = std::max(electricalQ, 0.35f);
  float w0 = kRadioTwoPi * std::max(suspensionHz, 1.0f);
  return std::sqrt(std::max(w0 * movingMassKg * voiceCoilResistanceOhms / q,
                            1e-9f));
}

void configureSpeakerElectromechanics(SpeakerSim& speaker,
                                      float fs,
                                      float nominalLoadOhms) {
  speaker.electricalSampleRate = fs;
  speaker.nominalLoadOhms = std::max(nominalLoadOhms, 1e-3f);
  if (speaker.voiceCoilResistanceOhms <= 0.0f) {
    speaker.voiceCoilResistanceOhms = 0.82f * speaker.nominalLoadOhms;
  }
  if (speaker.movingMassKg <= 0.0f) {
    speaker.movingMassKg = 0.014f;
  }
  if (speaker.suspensionComplianceMetersPerNewton <= 0.0f) {
    speaker.suspensionComplianceMetersPerNewton =
        deriveSpeakerCompliance(speaker.movingMassKg, speaker.suspensionHz);
  }
  if (speaker.mechanicalDampingNsPerMeter <= 0.0f) {
    if (speaker.mechanicalQ > 0.0f) {
      speaker.mechanicalDampingNsPerMeter = deriveSpeakerMechanicalDampingFromQ(
          speaker.movingMassKg, speaker.suspensionHz, speaker.mechanicalQ);
    } else {
      speaker.mechanicalDampingNsPerMeter = deriveSpeakerMechanicalDamping(
          speaker.movingMassKg, speaker.suspensionHz, speaker.suspensionQ);
    }
  }
  if (speaker.voiceCoilInductanceHenries <= 0.0f) {
    speaker.voiceCoilInductanceHenries = deriveSpeakerVoiceCoilInductance(
        speaker.voiceCoilResistanceOhms, speaker.topLpHz);
  }
  if (speaker.forceFactorBl <= 0.0f) {
    if (speaker.electricalQ > 0.0f) {
      speaker.forceFactorBl = deriveSpeakerForceFactorFromQ(
          speaker.movingMassKg, speaker.suspensionHz,
          speaker.voiceCoilResistanceOhms, speaker.electricalQ);
    } else {
      speaker.forceFactorBl =
          deriveSpeakerForceFactor(speaker.movingMassKg, speaker.suspensionHz,
                                   speaker.voiceCoilResistanceOhms);
    }
  }
  float loadSenseMs = 1.0f;
  speaker.loadSenseCoeff =
      std::exp(-1.0f / (std::max(fs, 1.0f) * (loadSenseMs / 1000.0f)));
  speaker.effectiveLoadOhms = speaker.nominalLoadOhms;
}

void resetSpeakerElectromechanics(SpeakerSim& speaker) {
  speaker.electricalCurrentAmps = 0.0f;
  speaker.coneVelocityMetersPerSecond = 0.0f;
  speaker.coneDisplacementMeters = 0.0f;
  speaker.backEmfVolts = 0.0f;
  speaker.loadSenseVoltage = 0.0f;
  speaker.loadSenseCurrent = 0.0f;
  speaker.effectiveLoadOhms = std::max(speaker.nominalLoadOhms, 1e-3f);
}

float asymmetricSoftLimit(float x, float positiveLimit, float negativeLimit) {
  if (x >= 0.0f) {
    float safeLimit = std::max(positiveLimit, 1e-6f);
    return softClip(x / safeLimit, 0.98f) * safeLimit;
  }
  float safeLimit = std::max(negativeLimit, 1e-6f);
  return softClip(x / safeLimit, 0.98f) * safeLimit;
}

void initSpeakerModel(SpeakerSim& speaker, float fs) {
  float suspensionHzDerived =
      speaker.suspensionHz * (1.0f + 0.45f * speaker.coneMassTolerance -
                              0.65f * speaker.suspensionComplianceTolerance);
  float coneBodyHzDerived =
      speaker.coneBodyHz * (1.0f + 0.22f * speaker.coneMassTolerance +
                            0.16f * speaker.voiceCoilTolerance);
  float breakupShift = speaker.breakupTolerance;
  float upperBreakupHzDerived =
      speaker.upperBreakupHz *
      (1.0f + 0.30f * speaker.coneMassTolerance + 0.48f * breakupShift);
  float coneDipHzDerived =
      speaker.coneDipHz *
      (1.0f + 0.18f * speaker.coneMassTolerance + 0.55f * breakupShift);
  float upperBreakupGainDbDerived =
      speaker.upperBreakupGainDb *
      std::clamp(1.0f + 0.35f * breakupShift, 0.25f, 1.75f);
  float coneDipGainDbDerived =
      speaker.coneDipGainDb *
      std::clamp(1.0f + 0.20f * breakupShift, 0.25f, 1.75f);
  speaker.suspensionRes.setPeaking(fs, suspensionHzDerived, speaker.suspensionQ,
                                   speaker.suspensionGainDb);
  speaker.coneBody.setPeaking(fs, coneBodyHzDerived, speaker.coneBodyQ,
                              speaker.coneBodyGainDb);
  if (speaker.upperBreakupHz > 0.0f && speaker.upperBreakupQ > 0.0f &&
      std::fabs(speaker.upperBreakupGainDb) > 1e-3f) {
    speaker.upperBreakup.setPeaking(
        fs, clampSpeakerFilterHz(fs, upperBreakupHzDerived),
        speaker.upperBreakupQ, upperBreakupGainDbDerived);
  } else {
    speaker.upperBreakup = Biquad{};
  }
  if (speaker.coneDipHz > 0.0f && speaker.coneDipQ > 0.0f &&
      std::fabs(speaker.coneDipGainDb) > 1e-3f) {
    speaker.coneDip.setPeaking(fs, clampSpeakerFilterHz(fs, coneDipHzDerived),
                               speaker.coneDipQ, coneDipGainDbDerived);
  } else {
    speaker.coneDip = Biquad{};
  }
  if (speaker.topLpHz > 0.0f) {
    float topLpHzDerived =
        speaker.topLpHz / (1.0f + 0.40f * speaker.voiceCoilTolerance);
    speaker.topLp.setLowpass(fs, clampSpeakerFilterHz(fs, topLpHzDerived),
                             speaker.filterQ);
  } else {
    speaker.topLp = Biquad{};
  }
  if (speaker.hfLossLpHz > 0.0f && speaker.hfLossDepth > 0.0f) {
    float hfLossLpHzDerived =
        speaker.hfLossLpHz /
        (1.0f + 0.35f * speaker.voiceCoilTolerance +
         0.45f * std::max(speaker.breakupTolerance, 0.0f));
    speaker.hfLossLp.setLowpass(fs, clampSpeakerFilterHz(fs, hfLossLpHzDerived),
                                kRadioBiquadQ);
  } else {
    speaker.hfLossLp = Biquad{};
  }
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
  y = speaker.upperBreakup.process(y);
  y = speaker.coneDip.process(y);
  if (speaker.hfLossLpHz > 0.0f && speaker.hfLossDepth > 0.0f) {
    float hfLossDepth = clampf(speaker.hfLossDepth, 0.0f, 1.0f);
    float rolledOff = speaker.hfLossLp.process(y);
    y += hfLossDepth * (rolledOff - y);
  }
  if (speaker.topLpHz > 0.0f) {
    y = speaker.topLp.process(y);
  }

  // The audible compliance/asymmetry should follow actual cone travel, not
  // instantaneous speaker voltage. The electrical branch in the power stage
  // now solves displacement explicitly, so use that state here.
  float excursionRefMm = std::max(speaker.excursionRef, 1e-6f);
  float signedExcursionMm = 1000.0f * speaker.coneDisplacementMeters;
  float excursionMagnitudeMm = std::fabs(signedExcursionMm);
  if (excursionMagnitudeMm > speaker.excursionEnv) {
    speaker.excursionEnv =
        speaker.excursionAtk * speaker.excursionEnv +
        (1.0f - speaker.excursionAtk) * excursionMagnitudeMm;
  } else {
    speaker.excursionEnv =
        speaker.excursionRel * speaker.excursionEnv +
        (1.0f - speaker.excursionRel) * excursionMagnitudeMm;
  }

  float excursionT =
      clampf(speaker.excursionEnv / excursionRefMm, 0.0f, 1.0f);
  float signedExcursionT =
      clampf(signedExcursionMm / excursionRefMm, -1.0f, 1.0f);
  float asymmetry =
      1.0f + clampf(speaker.asymBias, -0.75f, 0.75f) * signedExcursionT;
  float complianceGain =
      1.0f - speaker.complianceLossDepth * excursionT *
                 clampf(asymmetry, 0.35f, 1.65f);
  y *= std::max(0.70f, complianceGain);
  if (speaker.limit > 0.0f) {
    float asym = clampf(speaker.asymBias, -0.75f, 0.75f);
    float positiveLimit = std::max(speaker.limit * (1.0f - 0.35f * asym), 1e-6f);
    float negativeLimit = std::max(speaker.limit * (1.0f + 0.35f * asym), 1e-6f);
    clipped = (y > positiveLimit) || (y < -negativeLimit);
    return asymmetricSoftLimit(y, positiveLimit, negativeLimit);
  }
  clipped = false;
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
  float electricalFs = std::max(radio.power.internalSampleRate, radio.sampleRate);
  configureSpeakerElectromechanics(speakerStage.speaker, electricalFs,
                                   radio.power.outputLoadResistanceOhms);
  speakerStage.speaker.drive = speakerStage.drive;
  speakerStage.physicalDriveVolts = 0.0f;
}

void RadioSpeakerNode::reset(Radio1938& radio) {
  auto& speakerStage = radio.speakerStage;
  speakerStage.osPrev = 0.0f;
  speakerStage.physicalDriveVolts = 0.0f;
  speakerStage.osLpIn.reset();
  speakerStage.osLpOut.reset();
  resetSpeakerModel(speakerStage.speaker);
  resetSpeakerElectromechanics(speakerStage.speaker);
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
