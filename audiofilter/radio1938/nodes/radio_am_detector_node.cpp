#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

void prechargeUnityLowpass(Biquad& biquad, float dcLevel) {
  biquad.z1 = dcLevel * (1.0f - biquad.b0);
  biquad.z2 = dcLevel * (biquad.b2 - biquad.a2);
}

void primeDetectorWarmStart(AMDetector& detector,
                            float audioRect,
                            float delayedAvcThreshold) {
  detector.detectorNode = audioRect;
  detector.avcEnv =
      std::max(detector.detectorNode - delayedAvcThreshold, 0.0f);
  detector.avcRect = detector.avcEnv;
  detector.warmStartPending = false;
}

struct DetectorSolveResult {
  float detectorNode = 0.0f;
  float avcNode = 0.0f;
};

DetectorSolveResult solveDetectorAndAvcNodes(const AMDetector& detector,
                                             float dt,
                                             float audioRect,
                                             float delayedAvcThreshold) {
  float storageCapG =
      std::max(detector.detectorStorageCapFarads, 1e-12f) / dt;
  float detectorLeakG =
      1.0f / std::max(detector.audioDischargeResistanceOhms, 1e-6f);
  float sourceG = 0.0f;
  if (audioRect > detector.detectorNode) {
    sourceG = 1.0f / std::max(detector.audioChargeResistanceOhms, 1e-6f);
  }

  float avcFeedG = 0.0f;
  if (std::max(audioRect, detector.detectorNode) > delayedAvcThreshold) {
    avcFeedG = 1.0f / std::max(detector.avcChargeResistanceOhms, 1e-6f);
  }
  float avcLeakG = 1.0f / std::max(detector.avcDischargeResistanceOhms, 1e-6f);
  float avcCapG = std::max(detector.avcFilterCapFarads, 1e-12f) / dt;
  float a00 = storageCapG + detectorLeakG + sourceG + avcFeedG;
  float a01 = -avcFeedG;
  float a10 = -avcFeedG;
  float a11 = avcFeedG + avcLeakG + avcCapG;
  float b0 = storageCapG * detector.detectorNode + sourceG * audioRect -
             avcFeedG * delayedAvcThreshold;
  float b1 = avcCapG * detector.avcEnv + avcFeedG * delayedAvcThreshold;
  float det = a00 * a11 - a01 * a10;
  assert(std::fabs(det) >= 1e-12f);

  DetectorSolveResult result{};
  result.detectorNode = std::max((b0 * a11 - a01 * b1) / det, 0.0f);
  result.avcNode = std::max((a00 * b1 - b0 * a10) / det, 0.0f);
  return result;
}

}  // namespace

void AMDetector::init(float newFs, float newBw, float newTuneHz) {
  fs = newFs;
  bwHz = newBw;
  tuneOffsetHz = newTuneHz;
  float avcCap = avcFilterCapFarads;
  float avcChargeSeconds = avcChargeResistanceOhms * avcCap;
  float avcReleaseSeconds = avcDischargeResistanceOhms * avcCap;
  avcChargeCoeff = std::exp(-1.0f / (fs * avcChargeSeconds));
  avcReleaseCoeff = std::exp(-1.0f / (fs * avcReleaseSeconds));
  setBandwidth(newBw, newTuneHz);
  reset();
}

void AMDetector::setBandwidth(float newBw, float newTuneHz) {
  bwHz = newBw;
  tuneOffsetHz = newTuneHz;
  float ifCrackleTauSeconds =
      1.0f / (kRadioPi * std::max(bwHz, 1.0f));
  ifCrackleDecay =
      std::exp(-1.0f / (std::max(fs, 1.0f) *
                        std::max(ifCrackleTauSeconds, 1e-6f)));
  float lowSenseBound = std::max(40.0f, senseLowHz);
  float highSenseBound = std::max(lowSenseBound + 180.0f, senseHighHz);
  float ifCenter = 0.5f * (lowSenseBound + highSenseBound);
  float afcOffset =
      std::clamp(0.18f * (highSenseBound - lowSenseBound), 120.0f,
                 std::max(120.0f, 0.30f * (highSenseBound - lowSenseBound)));
  float lowSenseHz =
      std::clamp(ifCenter - afcOffset, lowSenseBound, highSenseBound - 180.0f);
  float highSenseHz =
      std::clamp(ifCenter + afcOffset, lowSenseHz + 120.0f, highSenseBound);
  float afcLpHz = std::clamp(0.30f * std::max(bwHz, 1.0f), 80.0f,
                             std::min(1800.0f, 0.12f * fs));
  afcLowOffsetHz = lowSenseHz - ifCenter;
  afcHighOffsetHz = highSenseHz - ifCenter;
  afcLowStep = kRadioTwoPi * (afcLowOffsetHz / std::max(fs, 1.0f));
  afcHighStep = kRadioTwoPi * (afcHighOffsetHz / std::max(fs, 1.0f));
  afcLowProbe.setLowpass(fs, afcLpHz, kRadioBiquadQ);
  afcHighProbe.setLowpass(fs, afcLpHz, kRadioBiquadQ);
  afcErrorLp.setLowpass(fs, afcLpHz, kRadioBiquadQ);
}

void AMDetector::setSenseWindow(float lowHz, float highHz) {
  senseLowHz = lowHz;
  senseHighHz = highHz;
  if (fs > 0.0f) {
    setBandwidth(bwHz, tuneOffsetHz);
  }
}

void AMDetector::reset() {
  audioRect = 0.0f;
  avcRect = 0.0f;
  detectorNode = 0.0f;
  avcEnv = 0.0f;
  warmStartPending = true;
  afcError = 0.0f;
  ifCrackleEnv = 0.0f;
  ifCracklePhase = 0.0f;
  ifCrackleEventCount = 0;
  ifCrackleMaxBurstAmp = 0.0f;
  ifCrackleMaxEnv = 0.0f;
  afcLowPhase = 0.0f;
  afcHighPhase = 0.0f;
  afcLowSense.reset();
  afcHighSense.reset();
  afcLowProbe.reset();
  afcHighProbe.reset();
  afcErrorLp.reset();
}

float AMDetector::run(float signalI,
                      float signalQ,
                      float ifNoiseAmp,
                      Radio1938& radio,
                      float ifCrackleAmp,
                      float ifCrackleRate) {
  constexpr float kInvSqrt2 = 0.70710678118f;
  float ifI = signalI;
  float ifQ = signalQ;
  if (ifNoiseAmp > 0.0f) {
    float noiseScale = ifNoiseAmp * kInvSqrt2;
    ifI += dist(rng) * noiseScale;
    ifQ += dist(rng) * noiseScale;
  }
  if (ifCrackleAmp > 0.0f && ifCrackleRate > 0.0f && fs > 0.0f) {
    float chance = std::min(ifCrackleRate / fs, 1.0f);
    float eventDraw = 0.5f * (dist(rng) + 1.0f);
    if (eventDraw < chance) {
      // Impulsive RF interference reaches the detector through the tuned IF
      // path, so model it as a short ring-down in the complex envelope instead
      // of a post-audio click.
      float burstPhase = kRadioPi * (dist(rng) + 1.0f);
      float burstAmpDraw = 0.5f * (dist(rng) + 1.0f);
      float burstAmp = ifCrackleAmp * (0.35f + 0.65f * burstAmpDraw);
      ifCrackleEnv = std::max(ifCrackleEnv, burstAmp);
      ifCrackleEventCount++;
      ifCrackleMaxBurstAmp = std::max(ifCrackleMaxBurstAmp, burstAmp);
      ifCracklePhase = burstPhase;
    }
  }
  if (ifCrackleEnv > 1e-6f) {
    ifCrackleMaxEnv = std::max(ifCrackleMaxEnv, ifCrackleEnv);
    ifI += ifCrackleEnv * std::cos(ifCracklePhase);
    ifQ += ifCrackleEnv * std::sin(ifCracklePhase);
    ifCrackleEnv *= ifCrackleDecay;
  }

  auto processProbe = [&](float phase,
                          float step,
                          IQBiquad& probe,
                          float& nextPhase) {
    float c = std::cos(phase);
    float s = std::sin(phase);
    float mixedI = ifI * c + ifQ * s;
    float mixedQ = ifQ * c - ifI * s;
    auto filtered = probe.process(mixedI, mixedQ);
    nextPhase = wrapPhase(phase + step);
    return std::sqrt(filtered[0] * filtered[0] + filtered[1] * filtered[1]);
  };

  float nextLowPhase = afcLowPhase;
  float nextHighPhase = afcHighPhase;
  float afcLow =
      processProbe(afcLowPhase, afcLowStep, afcLowProbe, nextLowPhase);
  float afcHigh =
      processProbe(afcHighPhase, afcHighStep, afcHighProbe, nextHighPhase);
  afcLowPhase = nextLowPhase;
  afcHighPhase = nextHighPhase;

  float afcDen = std::max(afcLow + afcHigh, 1e-6f);
  float rawAfcError = (afcHigh - afcLow) / afcDen;
  afcError = afcErrorLp.process(rawAfcError);

  float ifMagnitude = std::sqrt(ifI * ifI + ifQ * ifQ);
  audioRect = diodeJunctionRectify(ifMagnitude, audioDiodeDrop,
                                   audioJunctionSlopeVolts);
  float delayedAvcThreshold = 0.18f * std::max(controlVoltageRef, 1e-6f);
  float dt = 1.0f / std::max(fs, 1.0f);
  if (warmStartPending) {
    primeDetectorWarmStart(*this, audioRect, delayedAvcThreshold);
  }

  DetectorSolveResult solve =
      solveDetectorAndAvcNodes(*this, dt, audioRect, delayedAvcThreshold);
  detectorNode = solve.detectorNode;
  avcEnv = solve.avcNode;
  avcRect = avcEnv;
  assert(std::isfinite(detectorNode) && std::isfinite(avcEnv));
  if (radio.calibration.enabled) {
    radio.calibration.detectorNodeVolts.accumulate(detectorNode);
  }
  return audioRect;
}

namespace {

void ensureAMDetectorConfigured(Radio1938& radio) {
  auto& demod = radio.demod;
  const auto& ifStrip = radio.ifStrip;
  const auto& tuning = radio.tuning;
  uint32_t desiredRevision =
      ifStrip.enabled ? ifStrip.appliedConfigRevision : tuning.configRevision;
  if (demod.appliedConfigRevision == desiredRevision) return;

  if (ifStrip.enabled) {
    demod.am.setSenseWindow(ifStrip.senseLowHz, ifStrip.senseHighHz);
    demod.am.setBandwidth(ifStrip.demodBandwidthHz, ifStrip.demodTuneOffsetHz);
  } else {
    float senseHighHz = std::max(180.0f, 2.0f * tuning.tunedBw);
    demod.am.setSenseWindow(0.0f, senseHighHz);
    demod.am.setBandwidth(tuning.tunedBw, tuning.tuneAppliedHz);
  }
  demod.appliedConfigRevision = desiredRevision;
}

}  // namespace

void RadioAMDetectorNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& demod = radio.demod;
  demod.appliedConfigRevision = 0;
  demod.am.init(radio.sampleRate, initCtx.tunedBw, radio.tuning.tuneOffsetHz);
  ensureAMDetectorConfigured(radio);
}

void RadioAMDetectorNode::reset(Radio1938& radio) {
  radio.demod.am.reset();
  radio.demod.appliedConfigRevision = 0;
}

float RadioAMDetectorNode::run(Radio1938& radio,
                               float y,
                               RadioSampleContext& ctx) {
  auto& demod = radio.demod;
  ensureAMDetectorConfigured(radio);
  if (radio.ifStrip.enabled) {
    y = demod.am.run(ctx.signal.detectorInputI, ctx.signal.detectorInputQ,
                     ctx.derived.demodIfNoiseAmp, radio,
                     ctx.derived.demodIfCrackleAmp,
                     ctx.derived.demodIfCrackleRate);
  } else if (ctx.signal.mode == SourceInputMode::ComplexEnvelope) {
    y = demod.am.run(ctx.signal.i, ctx.signal.q, ctx.derived.demodIfNoiseAmp,
                     radio, ctx.derived.demodIfCrackleAmp,
                     ctx.derived.demodIfCrackleRate);
  } else {
    y = demod.am.run(y, 0.0f, ctx.derived.demodIfNoiseAmp, radio,
                     ctx.derived.demodIfCrackleAmp,
                     ctx.derived.demodIfCrackleRate);
  }
  radio.controlSense.controlVoltageSense =
      clampf(demod.am.avcEnv / std::max(demod.am.controlVoltageRef, 1e-6f),
             0.0f, 1.25f);
  radio.controlSense.tuningErrorSense = demod.am.afcError;
  return y;
}
