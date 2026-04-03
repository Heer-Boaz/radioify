#include "am_detector_internal.h"

#include "../../math/signal_math.h"
#include "../radio1938_constants.h"

#include <algorithm>
#include <cmath>

namespace am_detector_internal {

int detectorWaveformSubsteps(const AMDetector& detector,
                             float sampleRate,
                             float carrierHz) {
  if (!(sampleRate > 0.0f) || !(carrierHz > 0.0f)) return 1;
  int substeps = static_cast<int>(std::ceil(
      std::max(detector.waveformSamplesPerCycle, 1.0f) * carrierHz /
      sampleRate));
  return std::clamp(substeps, 1, std::max(detector.waveformMaxSubsteps, 1));
}

EnvelopeTrajectory buildEnvelopeTrajectory(bool useQuadratic,
                                           float prevPrevIfI,
                                           float prevPrevIfQ,
                                           float prevIfI,
                                           float prevIfQ,
                                           float ifI,
                                           float ifQ) {
  EnvelopeTrajectory trajectory{};
  trajectory.i0 = prevIfI;
  trajectory.q0 = prevIfQ;
  if (useQuadratic) {
    trajectory.i1 = 0.5f * (ifI - prevPrevIfI);
    trajectory.q1 = 0.5f * (ifQ - prevPrevIfQ);
    trajectory.i2 = 0.5f * (prevPrevIfI - 2.0f * prevIfI + ifI);
    trajectory.q2 = 0.5f * (prevPrevIfQ - 2.0f * prevIfQ + ifQ);
  } else {
    trajectory.i1 = ifI - prevIfI;
    trajectory.q1 = ifQ - prevIfQ;
  }
  return trajectory;
}

void evaluateEnvelopeAt(const EnvelopeTrajectory& trajectory,
                        float t,
                        float& envI,
                        float& envQ) {
  envI = trajectory.i0 + t * (trajectory.i1 + t * trajectory.i2);
  envQ = trajectory.q0 + t * (trajectory.q1 + t * trajectory.q2);
}

float sampleIfWave(const EnvelopeTrajectory& trajectory,
                   float t,
                   float c,
                   float s) {
  float envI = 0.0f;
  float envQ = 0.0f;
  evaluateEnvelopeAt(trajectory, t, envI, envQ);
  return envI * c - envQ * s;
}

struct DetectorIslandAccum {
  float audioRectArea = 0.0f;
  float avcRectArea = 0.0f;
  float detectorNodeArea = 0.0f;
  int solveSteps = 0;
  int intervalCount = 0;
  int splitIntervalCount = 0;
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
  detector.waveformSampleCount++;
  detector.lastWaveformSubsteps = substeps;
  if (substeps <= 1) {
    runEnvelopeDetectorPath(detector, std::sqrt(ifI * ifI + ifQ * ifQ),
                            detectorLeakG);
    detector.waveformIntervalCount += 1;
    detector.waveformSolveStepCount += 1;
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
  EnvelopeTrajectory trajectory =
      buildEnvelopeTrajectory(useQuadraticInterp, prevPrevIfI, prevPrevIfQ,
                              prevIfI, prevIfQ, ifI, ifQ);
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
  float ifWaveStart = sampleIfWave(trajectory, 0.0f, c, s);

  for (int step = 0; step < substeps; ++step) {
    float tMid =
        (static_cast<float>(step) + 0.5f) / static_cast<float>(substeps);
    float t1 = static_cast<float>(step + 1) / static_cast<float>(substeps);
    float cMid = c * cHalfStep - s * sHalfStep;
    float sMid = s * cHalfStep + c * sHalfStep;
    float nextC = c * cStep - s * sStep;
    float nextS = s * cStep + c * sStep;
    float ifWaveMid = sampleIfWave(trajectory, tMid, cMid, sMid);
    float ifWaveEnd = sampleIfWave(trajectory, t1, nextC, nextS);
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
      accum.intervalCount += 2;
      accum.splitIntervalCount += 1;
      float tQuarter = (static_cast<float>(step) + 0.25f) /
                       static_cast<float>(substeps);
      float tThreeQuarter = (static_cast<float>(step) + 0.75f) /
                            static_cast<float>(substeps);
      float cQuarter = c * cQuarterStep - s * sQuarterStep;
      float sQuarter = s * cQuarterStep + c * sQuarterStep;
      float cThreeQuarter = cMid * cQuarterStep - sMid * sQuarterStep;
      float sThreeQuarter = sMid * cQuarterStep + cMid * sQuarterStep;
      float ifWaveQuarter =
          sampleIfWave(trajectory, tQuarter, cQuarter, sQuarter);
      float ifWaveThreeQuarter =
          sampleIfWave(trajectory, tThreeQuarter, cThreeQuarter, sThreeQuarter);
      float halfDt = 0.5f * dtSub;
      float halfWidth = 0.5f * coarseIntervalWidth;
      accumulateDetectorInterval(detector, halfDt, halfWidth, ifWaveQuarter,
                                 delayedAvcThreshold, detectorLeakG, accum);
      accumulateDetectorInterval(detector, halfDt, halfWidth,
                                 ifWaveThreeQuarter, delayedAvcThreshold,
                                 detectorLeakG, accum);
    } else {
      accum.intervalCount += 1;
      accumulateDetectorInterval(detector, dtSub, coarseIntervalWidth,
                                 ifWaveMid, delayedAvcThreshold,
                                 detectorLeakG, accum);
    }
    ifWaveStart = ifWaveEnd;
    c = nextC;
    s = nextS;
  }

  detector.audioRect = accum.audioRectArea;
  detector.avcRect = accum.avcRectArea;
  detector.detectorNode = accum.detectorNodeArea;
  detector.lastWaveformSubsteps = accum.solveSteps;
  detector.waveformIntervalCount += static_cast<uint64_t>(accum.intervalCount);
  detector.waveformSplitIntervalCount +=
      static_cast<uint64_t>(accum.splitIntervalCount);
  detector.waveformSolveStepCount += static_cast<uint64_t>(accum.solveSteps);
  detector.ifWavePhase =
      wrapPhase(detector.ifWavePhase + kRadioTwoPi * (carrierHz / sampleRate));
  detector.prevPrevIfI = prevIfI;
  detector.prevPrevIfQ = prevIfQ;
  detector.prevIfI = ifI;
  detector.prevIfQ = ifQ;
  detector.ifEnvelopeHistorySamples =
      std::min(detector.ifEnvelopeHistorySamples + 1, 2);
}

}  // namespace am_detector_internal
