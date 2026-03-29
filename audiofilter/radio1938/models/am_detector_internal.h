#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_AM_DETECTOR_INTERNAL_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_AM_DETECTOR_INTERNAL_H

#include "am_detector.h"

#include <chrono>
#include <cstdint>

struct Radio1938;

namespace am_detector_internal {

inline uint64_t monotonicNowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct DetectorSolveResult {
  float detectorNode = 0.0f;
  float avcNode = 0.0f;
};

struct DetectorChargeBranch {
  float currentAmps = 0.0f;
  float conductanceSiemens = 0.0f;
};

struct EnvelopeTrajectory {
  float i0 = 0.0f;
  float i1 = 0.0f;
  float i2 = 0.0f;
  float q0 = 0.0f;
  float q1 = 0.0f;
  float q2 = 0.0f;
};

void primeDetectorWarmStart(AMDetector& detector,
                            float audioRect,
                            float avcDrive);

DetectorChargeBranch evaluateDetectorChargeBranch(float sourceVolts,
                                                  float nodeVolts,
                                                  float diodeDropVolts,
                                                  float junctionSlopeVolts,
                                                  float chargeResistanceOhms);

float solveDetectorAudioNode(AMDetector& detector,
                             float dt,
                             float sourceVolts,
                             float detectorLeakG);

DetectorSolveResult stepDetectorStorageAndAvc(AMDetector& detector,
                                              float dt,
                                              float detectorDriveVolts,
                                              float avcDrive,
                                              float detectorLeakG);

int detectorWaveformSubsteps(const AMDetector& detector,
                             float sampleRate,
                             float carrierHz);

EnvelopeTrajectory buildEnvelopeTrajectory(bool useQuadratic,
                                           float prevPrevIfI,
                                           float prevPrevIfQ,
                                           float prevIfI,
                                           float prevIfQ,
                                           float ifI,
                                           float ifQ);

void evaluateEnvelopeAt(const EnvelopeTrajectory& trajectory,
                        float t,
                        float& envI,
                        float& envQ);

float sampleIfWave(const EnvelopeTrajectory& trajectory,
                   float t,
                   float c,
                   float s);

void updateDetectorEnvelopeHistory(AMDetector& detector, float ifI, float ifQ);

void runEnvelopeDetectorPath(AMDetector& detector,
                             float ifMagnitude,
                             float detectorLeakG);

void runWaveformDetectorIsland(AMDetector& detector,
                               float ifI,
                               float ifQ,
                               float detectorLeakG,
                               float carrierHz);

}  // namespace am_detector_internal

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_AM_DETECTOR_INTERNAL_H
