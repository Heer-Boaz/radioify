#include "../radio.h"
#include "math/radio_math.h"
#include "receiver/receiver_input_network.h"

#include <algorithm>
#include <cassert>
#include <cmath>

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
  float audioPostLpHz = std::clamp(0.72f * std::max(bwHz, 1.0f), 1800.0f,
                                   std::min(7500.0f, 0.16f * fs));
  float ifCrackleTauSeconds =
      1.0f / (kRadioPi * std::max(bwHz, 1.0f));
  float ifCrackleDecay =
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
  audioPostLp1.setLowpass(fs, audioPostLpHz, kRadioBiquadQ);
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
  audioEnv = 0.0f;
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
  audioPostLp1.reset();
}

float AMDetector::processEnvelope(float signalI,
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
    detectorNode = audioRect;
    avcEnv = std::max(detectorNode - delayedAvcThreshold, 0.0f);
    avcRect = avcEnv;
    auto prechargeUnityLowpass = [](Biquad& biquad, float dcLevel) {
      biquad.z1 = dcLevel * (1.0f - biquad.b0);
      biquad.z2 = dcLevel * (biquad.b2 - biquad.a2);
    };
    prechargeUnityLowpass(audioPostLp1, detectorNode);
    warmStartPending = false;
  }
  float storageCapG =
      std::max(detectorStorageCapFarads, 1e-12f) / dt;
  float detectorLeakG =
      1.0f / std::max(audioDischargeResistanceOhms, 1e-6f);
  float sourceG = 0.0f;
  if (audioRect > detectorNode) {
    sourceG = 1.0f / std::max(audioChargeResistanceOhms, 1e-6f);
  }

  float avcFeedG = 0.0f;
  if (std::max(audioRect, detectorNode) > delayedAvcThreshold) {
    avcFeedG = 1.0f / std::max(avcChargeResistanceOhms, 1e-6f);
  }
  float avcLeakG = 1.0f / std::max(avcDischargeResistanceOhms, 1e-6f);
  float avcCapG = std::max(avcFilterCapFarads, 1e-12f) / dt;
  float a00 = storageCapG + detectorLeakG + sourceG + avcFeedG;
  float a01 = -avcFeedG;
  float a10 = -avcFeedG;
  float a11 = avcFeedG + avcLeakG + avcCapG;
  float b0 = storageCapG * detectorNode + sourceG * audioRect -
             avcFeedG * delayedAvcThreshold;
  float b1 = avcCapG * avcEnv + avcFeedG * delayedAvcThreshold;
  float solvedDetectorNode = detectorNode;
  float solvedAvcNode = avcEnv;
  if (radio.receiverCircuit.enabled) {
    a00 += computeReceiverDetectorLoadConductance(radio.receiverCircuit);
  }
  float det = a00 * a11 - a01 * a10;
  assert(std::fabs(det) >= 1e-12f);
  solvedDetectorNode = (b0 * a11 - a01 * b1) / det;
  solvedAvcNode = (a00 * b1 - b0 * a10) / det;
  detectorNode = std::max(solvedDetectorNode, 0.0f);
  avcEnv = std::max(solvedAvcNode, 0.0f);
  avcRect = avcEnv;
  assert(std::isfinite(detectorNode) && std::isfinite(avcEnv));
  if (radio.calibration.enabled) {
    radio.calibration.detectorNodeVolts.accumulate(detectorNode);
  }

  audioEnv = audioPostLp1.process(detectorNode);
  return audioEnv;
}

float AMDetector::process(const AMDetectorSampleInput& in, Radio1938& radio) {
  return processEnvelope(in.signal, 0.0f, in.ifNoiseAmp, radio,
                         in.ifCrackleAmp, in.ifCrackleRate);
}
