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
    speaker.mechanicalDampingNsPerMeter = deriveSpeakerMechanicalDamping(
        speaker.movingMassKg, speaker.suspensionHz, speaker.suspensionQ);
  }
  if (speaker.voiceCoilInductanceHenries <= 0.0f) {
    speaker.voiceCoilInductanceHenries = deriveSpeakerVoiceCoilInductance(
        speaker.voiceCoilResistanceOhms, speaker.topLpHz);
  }
  if (speaker.forceFactorBl <= 0.0f) {
    speaker.forceFactorBl =
        deriveSpeakerForceFactor(speaker.movingMassKg, speaker.suspensionHz,
                                 speaker.voiceCoilResistanceOhms);
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

void advanceSpeakerElectromechanics(SpeakerSim& speaker, float appliedVolts) {
  float fs = std::max(speaker.electricalSampleRate, 1.0f);
  float dt = 1.0f / fs;
  float re = std::max(speaker.voiceCoilResistanceOhms, 1e-4f);
  float le = std::max(speaker.voiceCoilInductanceHenries, 1e-7f);
  float mass = std::max(speaker.movingMassKg, 1e-6f);
  float compliance =
      std::max(speaker.suspensionComplianceMetersPerNewton, 1e-8f);
  float damping = std::max(speaker.mechanicalDampingNsPerMeter, 1e-5f);
  float bl = std::max(speaker.forceFactorBl, 1e-4f);

  float i0 = speaker.electricalCurrentAmps;
  float v0 = speaker.coneVelocityMetersPerSecond;
  float x0 = speaker.coneDisplacementMeters;
  float diDt = (appliedVolts - re * i0 - bl * v0) / le;
  float i1 = i0 + dt * diDt;
  float forceNewtons = bl * i1;
  float accel =
      (forceNewtons - damping * v0 - (x0 / compliance)) / mass;
  float v1 = v0 + dt * accel;
  float x1 = x0 + dt * v1;
  float backEmf = bl * v1;

  speaker.electricalCurrentAmps = i1;
  speaker.coneVelocityMetersPerSecond = v1;
  speaker.coneDisplacementMeters = x1;
  speaker.backEmfVolts = backEmf;

  float coeff = clampf(speaker.loadSenseCoeff, 0.0f, 0.99999f);
  speaker.loadSenseVoltage =
      coeff * speaker.loadSenseVoltage + (1.0f - coeff) * std::fabs(appliedVolts);
  speaker.loadSenseCurrent = coeff * speaker.loadSenseCurrent +
                             (1.0f - coeff) * std::fabs(i1);
  if (speaker.loadSenseCurrent > 1e-5f && speaker.loadSenseVoltage > 1e-5f) {
    float minLoad = 0.70f * std::max(speaker.nominalLoadOhms, 1e-3f);
    float maxLoad = 4.5f * std::max(speaker.nominalLoadOhms, 1e-3f);
    speaker.effectiveLoadOhms = std::clamp(
        speaker.loadSenseVoltage / speaker.loadSenseCurrent, minLoad, maxLoad);
  }
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
  float signedExcursionT =
      clampf(y / std::max(speaker.excursionRef, 1e-6f), -1.0f, 1.0f);
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
  configureSpeakerElectromechanics(speakerStage.speaker, radio.sampleRate,
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
  advanceSpeakerElectromechanics(speakerStage.speaker,
                                 speakerStage.physicalDriveVolts);
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
