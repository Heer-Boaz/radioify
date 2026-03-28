#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

void primeDetectorWarmStart(AMDetector& detector,
                            float audioRect,
                            float avcDrive) {
  detector.detectorStorageNode = audioRect;
  detector.detectorNode = audioRect;
  detector.avcEnv = avcDrive;
  detector.avcRect = detector.avcEnv;
  detector.warmStartPending = false;
}

struct DetectorSolveResult {
  float detectorNode = 0.0f;
  float avcNode = 0.0f;
};

struct DetectorChargeBranch {
  float currentAmps = 0.0f;
  float conductanceSiemens = 0.0f;
};

DetectorChargeBranch evaluateDetectorChargeBranch(float sourceVolts,
                                                  float nodeVolts,
                                                  float diodeDropVolts,
                                                  float junctionSlopeVolts,
                                                  float chargeResistanceOhms) {
  float safeSlope = requirePositiveFinite(junctionSlopeVolts);
  float safeResistance = requirePositiveFinite(chargeResistanceOhms);
  float availableVolts = sourceVolts - nodeVolts - diodeDropVolts;
  float x = availableVolts / safeSlope;
  float conductionT = 0.0f;
  if (x >= 20.0f) {
    conductionT = 1.0f;
  } else if (x > -20.0f) {
    conductionT = 1.0f / (1.0f + std::exp(-x));
  }

  DetectorChargeBranch branch{};
  if (availableVolts > 0.0f) {
    branch.currentAmps = (availableVolts * conductionT) / safeResistance;
    branch.conductanceSiemens =
        (conductionT +
         availableVolts * conductionT * (1.0f - conductionT) / safeSlope) /
        safeResistance;
  }
  return branch;
}

float solveDetectorAudioNode(const AMDetector& detector,
                             float dt,
                             float sourceVolts,
                             float detectorLeakG) {
  float storageCapG =
      std::max(detector.detectorStorageCapFarads, 1e-12f) / dt;
  float nodeVolts = std::max(detector.detectorStorageNode, 0.0f);

  for (int iter = 0; iter < 8; ++iter) {
    DetectorChargeBranch branch = evaluateDetectorChargeBranch(
        sourceVolts, nodeVolts, detector.audioDiodeDrop,
        detector.audioJunctionSlopeVolts, detector.audioChargeResistanceOhms);
    float f = storageCapG * (nodeVolts - detector.detectorStorageNode) +
              detectorLeakG * nodeVolts - branch.currentAmps;
    float df = storageCapG + detectorLeakG + branch.conductanceSiemens;
    assert(std::isfinite(df) && df > 1e-12f);
    float delta = f / df;
    nodeVolts = std::max(nodeVolts - delta, 0.0f);
    if (std::fabs(delta) < 1e-7f) break;
  }

  return nodeVolts;
}

DetectorSolveResult solveDetectorAndAvcNodes(const AMDetector& detector,
                                             float dt,
                                             float detectorDriveVolts,
                                             float avcDrive,
                                             float detectorLeakG) {
  float avcSourceG = 0.0f;
  if (avcDrive > detector.avcEnv) {
    avcSourceG = 1.0f / std::max(detector.avcChargeResistanceOhms, 1e-6f);
  }
  float avcLeakG = 1.0f / std::max(detector.avcDischargeResistanceOhms, 1e-6f);
  float avcCapG = std::max(detector.avcFilterCapFarads, 1e-12f) / dt;
  float a11 = avcSourceG + avcLeakG + avcCapG;
  float b1 = avcCapG * detector.avcEnv + avcSourceG * avcDrive;

  DetectorSolveResult result{};
  result.detectorNode =
      solveDetectorAudioNode(detector, dt, detectorDriveVolts, detectorLeakG);
  result.avcNode = std::max(b1 / std::max(a11, 1e-12f), 0.0f);
  return result;
}

}  // namespace

namespace {

constexpr float kDetectorWaveformSamplesPerCycle = 3.0f;
constexpr int kDetectorWaveformMaxSubsteps = 32;

float effectiveDetectorLoadConductance(const Radio1938& radio) {
  float dischargeG =
      1.0f / std::max(radio.demod.am.audioDischargeResistanceOhms, 1e-6f);
  if (!radio.receiverCircuit.enabled) return dischargeG;
  return dischargeG + radio.receiverCircuit.detectorLoadConductance;
}

float detectorWaveformCarrierHz(const AMDetector& detector,
                                const Radio1938& radio) {
  if (!radio.ifStrip.enabled) return 0.0f;
  return std::fabs(radio.ifStrip.ifCenterHz + detector.tuneOffsetHz);
}

int detectorWaveformSubsteps(float sampleRate, float carrierHz) {
  if (!(sampleRate > 0.0f) || !(carrierHz > 0.0f)) return 1;
  int substeps = static_cast<int>(std::ceil(
      kDetectorWaveformSamplesPerCycle * carrierHz / sampleRate));
  return std::clamp(substeps, 1, kDetectorWaveformMaxSubsteps);
}

void runWaveformDetectorIsland(AMDetector& detector,
                               float ifI,
                               float ifQ,
                               float detectorLeakG,
                               float carrierHz) {
  float sampleRate = std::max(detector.fs, 1.0f);
  int substeps = detectorWaveformSubsteps(sampleRate, carrierHz);
  if (substeps <= 1) {
    float ifMagnitude = std::sqrt(ifI * ifI + ifQ * ifQ);
    detector.audioRect = diodeJunctionRectify(ifMagnitude, detector.audioDiodeDrop,
                                              detector.audioJunctionSlopeVolts);
    float avcRectified = diodeJunctionRectify(ifMagnitude, detector.avcDiodeDrop,
                                              detector.avcJunctionSlopeVolts);
    float delayedAvcThreshold =
        0.18f * std::max(detector.controlVoltageRef, 1e-6f);
    float avcDrive = std::max(avcRectified - delayedAvcThreshold, 0.0f);
    float dt = 1.0f / sampleRate;
    if (detector.warmStartPending) {
      primeDetectorWarmStart(detector, detector.audioRect, avcDrive);
    }
    DetectorSolveResult solve =
        solveDetectorAndAvcNodes(detector, dt, ifMagnitude, avcDrive,
                                 detectorLeakG);
    detector.detectorStorageNode = solve.detectorNode;
    detector.detectorNode = solve.detectorNode;
    detector.avcEnv = solve.avcNode;
    detector.avcRect = avcDrive;
    detector.prevIfI = ifI;
    detector.prevIfQ = ifQ;
    return;
  }

  float prevIfI = detector.prevIfI;
  float prevIfQ = detector.prevIfQ;
  if (detector.warmStartPending) {
    prevIfI = ifI;
    prevIfQ = ifQ;
  }
  float dtSub = 1.0f / (sampleRate * static_cast<float>(substeps));
  float phaseStep = kRadioTwoPi * (carrierHz / (sampleRate * substeps));
  float c = std::cos(detector.ifWavePhase);
  float s = std::sin(detector.ifWavePhase);
  float cStep = std::cos(phaseStep);
  float sStep = std::sin(phaseStep);
  float delayedAvcThreshold =
      0.18f * std::max(detector.controlVoltageRef, 1e-6f);
  float audioRectSum = 0.0f;
  float avcRectSum = 0.0f;
  float detectorNodeSum = 0.0f;

  for (int step = 0; step < substeps; ++step) {
    float t = (static_cast<float>(step) + 0.5f) / static_cast<float>(substeps);
    float envI = prevIfI + (ifI - prevIfI) * t;
    float envQ = prevIfQ + (ifQ - prevIfQ) * t;
    float ifWave = envI * c - envQ * s;
    detector.audioRect = diodeJunctionRectify(
        ifWave, detector.audioDiodeDrop, detector.audioJunctionSlopeVolts);
    float avcRectified = diodeJunctionRectify(
        ifWave, detector.avcDiodeDrop, detector.avcJunctionSlopeVolts);
    float avcDrive = std::max(avcRectified - delayedAvcThreshold, 0.0f);
    if (detector.warmStartPending) {
      primeDetectorWarmStart(detector, detector.audioRect, avcDrive);
    }
    DetectorSolveResult solve =
        solveDetectorAndAvcNodes(detector, dtSub, ifWave, avcDrive,
                                 detectorLeakG);
    detector.detectorStorageNode = solve.detectorNode;
    detector.avcEnv = solve.avcNode;
    audioRectSum += detector.audioRect;
    avcRectSum += avcDrive;
    detectorNodeSum += solve.detectorNode;

    float nextC = c * cStep - s * sStep;
    float nextS = s * cStep + c * sStep;
    c = nextC;
    s = nextS;
  }

  detector.audioRect = audioRectSum / static_cast<float>(substeps);
  detector.avcRect = avcRectSum / static_cast<float>(substeps);
  detector.detectorNode = detectorNodeSum / static_cast<float>(substeps);
  detector.ifWavePhase =
      wrapPhase(detector.ifWavePhase + kRadioTwoPi * (carrierHz / sampleRate));
  detector.prevIfI = ifI;
  detector.prevIfQ = ifQ;
}

}  // namespace

void AMDetector::init(float newFs, float newBw, float newTuneHz) {
  fs = newFs;
  bwHz = newBw;
  tuneOffsetHz = newTuneHz;
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
  float afcLpHz = afcSenseLpHz;
  if (!(afcLpHz > 0.0f)) {
    afcLpHz = 0.30f * std::max(bwHz, 1.0f);
  }
  afcLpHz = std::clamp(afcLpHz, 1.0f, std::min(1800.0f, 0.12f * fs));
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
  detectorStorageNode = 0.0f;
  detectorNode = 0.0f;
  avcEnv = 0.0f;
  ifWavePhase = 0.0f;
  prevIfI = 0.0f;
  prevIfQ = 0.0f;
  warmStartPending = true;
  afcError = 0.0f;
  ifCrackleEnv = 0.0f;
  ifCracklePhase = 0.0f;
  ifCrackleEventCount = 0;
  ifCrackleMaxBurstAmp = 0.0f;
  ifCrackleMaxEnv = 0.0f;
  afcLowPhase = 0.0f;
  afcHighPhase = 0.0f;
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

  float detectorLeakG = effectiveDetectorLoadConductance(radio);
  float carrierHz = detectorWaveformCarrierHz(*this, radio);
  if (carrierHz > 0.0f) {
    runWaveformDetectorIsland(*this, ifI, ifQ, detectorLeakG, carrierHz);
  } else {
    float ifMagnitude = std::sqrt(ifI * ifI + ifQ * ifQ);
    audioRect = diodeJunctionRectify(ifMagnitude, audioDiodeDrop,
                                     audioJunctionSlopeVolts);
    float avcRectified = diodeJunctionRectify(ifMagnitude, avcDiodeDrop,
                                              avcJunctionSlopeVolts);
    float delayedAvcThreshold = 0.18f * std::max(controlVoltageRef, 1e-6f);
    float avcDrive = std::max(avcRectified - delayedAvcThreshold, 0.0f);
    float dt = 1.0f / std::max(fs, 1.0f);
    if (warmStartPending) {
      primeDetectorWarmStart(*this, audioRect, avcDrive);
    }

    DetectorSolveResult solve =
        solveDetectorAndAvcNodes(*this, dt, ifMagnitude, avcDrive,
                                 detectorLeakG);
    detectorStorageNode = solve.detectorNode;
    detectorNode = solve.detectorNode;
    avcEnv = solve.avcNode;
    avcRect = avcDrive;
    prevIfI = ifI;
    prevIfQ = ifQ;
  }
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
