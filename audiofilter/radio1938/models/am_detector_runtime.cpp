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

DetectorSolveResult stepDetectorStorageAndAvc(const AMDetector& detector,
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

int detectorWaveformSubsteps(const AMDetector& detector,
                             float sampleRate,
                             float carrierHz) {
  if (!(sampleRate > 0.0f) || !(carrierHz > 0.0f)) return 1;
  int substeps = static_cast<int>(std::ceil(
      std::max(detector.waveformSamplesPerCycle, 1.0f) * carrierHz / sampleRate));
  return std::clamp(substeps, 1, std::max(detector.waveformMaxSubsteps, 1));
}

float interpolateEnvelopeLinear(float prevSample, float currentSample, float t) {
  return prevSample + (currentSample - prevSample) * t;
}

float interpolateEnvelopeQuadratic(float prevPrevSample,
                                   float prevSample,
                                   float currentSample,
                                   float t) {
  float slope = 0.5f * (currentSample - prevPrevSample);
  float curvature =
      0.5f * (prevPrevSample - 2.0f * prevSample + currentSample);
  return prevSample + slope * t + curvature * t * t;
}

float interpolateEnvelopeSample(bool useQuadratic,
                                float prevPrevSample,
                                float prevSample,
                                float currentSample,
                                float t) {
  return useQuadratic
             ? interpolateEnvelopeQuadratic(prevPrevSample, prevSample,
                                            currentSample, t)
             : interpolateEnvelopeLinear(prevSample, currentSample, t);
}

float sampleIfWave(bool useQuadratic,
                   float prevPrevIfI,
                   float prevPrevIfQ,
                   float prevIfI,
                   float prevIfQ,
                   float ifI,
                   float ifQ,
                   float t,
                   float c,
                   float s) {
  float envI =
      interpolateEnvelopeSample(useQuadratic, prevPrevIfI, prevIfI, ifI, t);
  float envQ =
      interpolateEnvelopeSample(useQuadratic, prevPrevIfQ, prevIfQ, ifQ, t);
  return envI * c - envQ * s;
}

struct DetectorIslandAccum {
  float audioRectArea = 0.0f;
  float avcRectArea = 0.0f;
  float detectorNodeArea = 0.0f;
  int solveSteps = 0;
};

void accumulateDetectorInterval(AMDetector& detector,
                                float dt,
                                float intervalWidth,
                                float ifWave,
                                float delayedAvcThreshold,
                                float detectorLeakG,
                                DetectorIslandAccum& accum) {
  detector.audioRect = diodeJunctionRectify(
      ifWave, detector.audioDiodeDrop, detector.audioJunctionSlopeVolts);
  float avcRectified = diodeJunctionRectify(
      ifWave, detector.avcDiodeDrop, detector.avcJunctionSlopeVolts);
  float avcDrive = std::max(avcRectified - delayedAvcThreshold, 0.0f);
  if (detector.warmStartPending) {
    primeDetectorWarmStart(detector, detector.audioRect, avcDrive);
  }
  float prevNode = std::max(detector.detectorStorageNode, 0.0f);
  DetectorSolveResult solve = stepDetectorStorageAndAvc(
      detector, dt, ifWave, avcDrive, detectorLeakG);
  detector.detectorStorageNode = solve.detectorNode;
  detector.avcEnv = solve.avcNode;
  accum.audioRectArea += detector.audioRect * intervalWidth;
  accum.avcRectArea += avcDrive * intervalWidth;
  accum.detectorNodeArea +=
      0.5f * (prevNode + solve.detectorNode) * intervalWidth;
  accum.solveSteps += 1;
}

void updateDetectorEnvelopeHistory(AMDetector& detector, float ifI, float ifQ) {
  detector.prevPrevIfI = detector.prevIfI;
  detector.prevPrevIfQ = detector.prevIfQ;
  detector.prevIfI = ifI;
  detector.prevIfQ = ifQ;
  detector.ifEnvelopeHistorySamples =
      std::min(detector.ifEnvelopeHistorySamples + 1, 2);
}

void runEnvelopeDetectorPath(AMDetector& detector,
                             float ifMagnitude,
                             float detectorLeakG) {
  detector.lastWaveformSubsteps = 1;
  detector.audioRect = diodeJunctionRectify(ifMagnitude, detector.audioDiodeDrop,
                                            detector.audioJunctionSlopeVolts);
  float avcRectified = diodeJunctionRectify(ifMagnitude, detector.avcDiodeDrop,
                                            detector.avcJunctionSlopeVolts);
  float delayedAvcThreshold =
      0.18f * std::max(detector.controlVoltageRef, 1e-6f);
  float avcDrive = std::max(avcRectified - delayedAvcThreshold, 0.0f);
  float dt = 1.0f / std::max(detector.fs, 1.0f);
  if (detector.warmStartPending) {
    primeDetectorWarmStart(detector, detector.audioRect, avcDrive);
  }
  DetectorSolveResult solve =
      stepDetectorStorageAndAvc(detector, dt, ifMagnitude, avcDrive,
                                detectorLeakG);
  detector.detectorStorageNode = solve.detectorNode;
  detector.detectorNode = solve.detectorNode;
  detector.avcEnv = solve.avcNode;
  detector.avcRect = avcDrive;
}

void runWaveformDetectorIsland(AMDetector& detector,
                               float ifI,
                               float ifQ,
                               float detectorLeakG,
                               float carrierHz) {
  float sampleRate = std::max(detector.fs, 1.0f);
  int substeps = detectorWaveformSubsteps(detector, sampleRate, carrierHz);
  detector.lastWaveformSubsteps = substeps;
  if (substeps <= 1) {
    runEnvelopeDetectorPath(detector, std::sqrt(ifI * ifI + ifQ * ifQ),
                            detectorLeakG);
    updateDetectorEnvelopeHistory(detector, ifI, ifQ);
    return;
  }

  float prevPrevIfI = detector.prevPrevIfI;
  float prevPrevIfQ = detector.prevPrevIfQ;
  float prevIfI = detector.prevIfI;
  float prevIfQ = detector.prevIfQ;
  if (detector.warmStartPending) {
    prevPrevIfI = ifI;
    prevPrevIfQ = ifQ;
    prevIfI = ifI;
    prevIfQ = ifQ;
  }
  bool useQuadraticInterp = detector.ifEnvelopeHistorySamples >= 2;
  float dtSub = 1.0f / (sampleRate * static_cast<float>(substeps));
  float phaseStep = kRadioTwoPi * (carrierHz / (sampleRate * substeps));
  float c = std::cos(detector.ifWavePhase);
  float s = std::sin(detector.ifWavePhase);
  float cHalfStep = std::cos(0.5f * phaseStep);
  float sHalfStep = std::sin(0.5f * phaseStep);
  float cQuarterStep = std::cos(0.25f * phaseStep);
  float sQuarterStep = std::sin(0.25f * phaseStep);
  float cStep = std::cos(phaseStep);
  float sStep = std::sin(phaseStep);
  float delayedAvcThreshold =
      0.18f * std::max(detector.controlVoltageRef, 1e-6f);
  float coarseIntervalWidth = 1.0f / static_cast<float>(substeps);
  DetectorIslandAccum accum{};

  for (int step = 0; step < substeps; ++step) {
    float t0 = static_cast<float>(step) / static_cast<float>(substeps);
    float tMid =
        (static_cast<float>(step) + 0.5f) / static_cast<float>(substeps);
    float t1 = static_cast<float>(step + 1) / static_cast<float>(substeps);
    float cMid = c * cHalfStep - s * sHalfStep;
    float sMid = s * cHalfStep + c * sHalfStep;
    float nextC = c * cStep - s * sStep;
    float nextS = s * cStep + c * sStep;
    float ifWaveStart =
        sampleIfWave(useQuadraticInterp, prevPrevIfI, prevPrevIfQ, prevIfI,
                     prevIfQ, ifI, ifQ, t0, c, s);
    float ifWaveMid =
        sampleIfWave(useQuadraticInterp, prevPrevIfI, prevPrevIfQ, prevIfI,
                     prevIfQ, ifI, ifQ, tMid, cMid, sMid);
    float ifWaveEnd =
        sampleIfWave(useQuadraticInterp, prevPrevIfI, prevPrevIfQ, prevIfI,
                     prevIfQ, ifI, ifQ, t1, nextC, nextS);
    float audioForwardThreshold =
        std::max(detector.detectorStorageNode, 0.0f) + detector.audioDiodeDrop;
    float avcForwardThreshold = std::max(detector.avcEnv, 0.0f) +
                                detector.avcDiodeDrop +
                                delayedAvcThreshold;
    float forwardThreshold =
        std::min(audioForwardThreshold, avcForwardThreshold);
    float ifWavePeak =
        std::max(ifWaveStart, std::max(ifWaveMid, ifWaveEnd));
    bool shouldSplit = ifWavePeak > forwardThreshold;

    if (shouldSplit) {
      float tQuarter = (static_cast<float>(step) + 0.25f) /
                       static_cast<float>(substeps);
      float tThreeQuarter = (static_cast<float>(step) + 0.75f) /
                            static_cast<float>(substeps);
      float cQuarter = c * cQuarterStep - s * sQuarterStep;
      float sQuarter = s * cQuarterStep + c * sQuarterStep;
      float cThreeQuarter = cMid * cQuarterStep - sMid * sQuarterStep;
      float sThreeQuarter = sMid * cQuarterStep + cMid * sQuarterStep;
      float ifWaveQuarter =
          sampleIfWave(useQuadraticInterp, prevPrevIfI, prevPrevIfQ, prevIfI,
                       prevIfQ, ifI, ifQ, tQuarter, cQuarter, sQuarter);
      float ifWaveThreeQuarter =
          sampleIfWave(useQuadraticInterp, prevPrevIfI, prevPrevIfQ, prevIfI,
                       prevIfQ, ifI, ifQ, tThreeQuarter, cThreeQuarter,
                       sThreeQuarter);
      float halfDt = 0.5f * dtSub;
      float halfWidth = 0.5f * coarseIntervalWidth;
      accumulateDetectorInterval(detector, halfDt, halfWidth, ifWaveQuarter,
                                 delayedAvcThreshold, detectorLeakG, accum);
      accumulateDetectorInterval(detector, halfDt, halfWidth,
                                 ifWaveThreeQuarter, delayedAvcThreshold,
                                 detectorLeakG, accum);
    } else {
      accumulateDetectorInterval(detector, dtSub, coarseIntervalWidth,
                                 ifWaveMid, delayedAvcThreshold,
                                 detectorLeakG, accum);
    }
    c = nextC;
    s = nextS;
  }

  detector.audioRect = accum.audioRectArea;
  detector.avcRect = accum.avcRectArea;
  detector.detectorNode = accum.detectorNodeArea;
  detector.lastWaveformSubsteps = accum.solveSteps;
  detector.ifWavePhase =
      wrapPhase(detector.ifWavePhase + kRadioTwoPi * (carrierHz / sampleRate));
  detector.prevPrevIfI = prevIfI;
  detector.prevPrevIfQ = prevIfQ;
  detector.prevIfI = ifI;
  detector.prevIfQ = ifQ;
  detector.ifEnvelopeHistorySamples =
      std::min(detector.ifEnvelopeHistorySamples + 1, 2);
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
  prevPrevIfI = 0.0f;
  prevPrevIfQ = 0.0f;
  prevIfI = 0.0f;
  prevIfQ = 0.0f;
  ifEnvelopeHistorySamples = 0;
  lastWaveformSubsteps = 0;
  warmStartPending = true;
  afcError = 0.0f;
  ifCrackleEnv = 0.0f;
  ifCrackleDecay = 0.0f;
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
    runEnvelopeDetectorPath(*this, std::sqrt(ifI * ifI + ifQ * ifQ),
                            detectorLeakG);
    updateDetectorEnvelopeHistory(*this, ifI, ifQ);
  }
  assert(std::isfinite(detectorNode) && std::isfinite(avcEnv));
  if (radio.calibration.enabled) {
    radio.calibration.detectorNodeVolts.accumulate(detectorNode);
  }
  return audioRect;
}
