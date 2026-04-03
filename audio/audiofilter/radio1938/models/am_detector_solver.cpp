#include "am_detector_internal.h"

#include "../../math/signal_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace am_detector_internal {

void primeDetectorWarmStart(AMDetector& detector,
                            float audioRect,
                            float avcDrive) {
  detector.detectorStorageNode = audioRect;
  detector.detectorNode = audioRect;
  detector.avcEnv = avcDrive;
  detector.avcRect = detector.avcEnv;
  detector.warmStartPending = false;
}

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

float solveDetectorAudioNode(AMDetector& detector,
                             float dt,
                             float sourceVolts,
                             float detectorLeakG) {
  uint64_t startNs = detector.metricsEnabled ? monotonicNowNs() : 0;
  float storageCapG =
      std::max(detector.detectorStorageCapFarads, 1e-12f) / dt;
  float previousNode = std::max(detector.detectorStorageNode, 0.0f);
  float nodeVolts = previousNode;
  int iterations = 0;

  if (sourceVolts <= detector.audioDiodeDrop) {
    detector.storageSolveCallCount++;
    if (detector.metricsEnabled) {
      detector.storageSolveTimeNs += monotonicNowNs() - startNs;
    }
    return (storageCapG * previousNode) /
           std::max(storageCapG + detectorLeakG, 1e-12f);
  }

  for (int iter = 0; iter < 8; ++iter) {
    iterations = iter + 1;
    DetectorChargeBranch branch = evaluateDetectorChargeBranch(
        sourceVolts, nodeVolts, detector.audioDiodeDrop,
        detector.audioJunctionSlopeVolts, detector.audioChargeResistanceOhms);
    float f = storageCapG * (nodeVolts - previousNode) +
              detectorLeakG * nodeVolts - branch.currentAmps;
    float df = storageCapG + detectorLeakG + branch.conductanceSiemens;
    assert(std::isfinite(df) && df > 1e-12f);
    float delta = f / df;
    nodeVolts = std::max(nodeVolts - delta, 0.0f);
    if (std::fabs(delta) < 1e-7f) break;
  }

  detector.storageSolveCallCount++;
  detector.storageSolveIterationCount += static_cast<uint64_t>(iterations);
  detector.storageSolveMaxIterations =
      std::max(detector.storageSolveMaxIterations,
               static_cast<uint32_t>(iterations));
  if (detector.metricsEnabled) {
    detector.storageSolveTimeNs += monotonicNowNs() - startNs;
  }

  return nodeVolts;
}

DetectorSolveResult stepDetectorStorageAndAvc(AMDetector& detector,
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

}  // namespace am_detector_internal
