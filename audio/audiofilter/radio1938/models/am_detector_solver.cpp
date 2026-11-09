#include "am_detector_internal.h"

#include <algorithm>
#include <cmath>

namespace am_detector_internal {
namespace {
// DC equilibrium of the periodic detector, only used when starting reception.
// Both signal and thermal contact currents charge the actual RC load.
double equilibrium(const ThermionicDiode& diode, double peak, double bias, double sourceR,
                   DetectorLoad load, double filterR = 0) {
  double low = 0, high = peak + 2 + std::abs(diode.contactPotentialVolts);
  for (int iteration = 0; iteration < 36; ++iteration) {
    const double v = .5 * (low + high);
    double current = 0;
    for (int n = 0; n < 64; ++n)
      current +=
          diode.current(peak * std::cos(6.283185307179586 * (n + .5) / 64) - bias - v -
                            filterR * (load.conductance * v + load.historyCurrent),
                        sourceR) /
          64;
    if (current > load.conductance * v + load.historyCurrent)
      low = v;
    else
      high = v;
  }
  return .5 * (low + high);
}

float solveStorage(const ThermionicDiode& diode, double dt, double source, double sourceR,
                   double capacitance, double previous, DetectorLoad load,
                   int* iterations = nullptr) {
  const double capG = capacitance / dt;
  const double totalG = capG + load.conductance;
  const double unloaded = (capG * previous - load.historyCurrent) / totalG;
  const double current =
      diode.current(source - unloaded, sourceR + 1 / totalG, iterations);
  return static_cast<float>(unloaded + current / totalG);
}
}  // namespace

void primeDetectorWarmStart(AMDetector& detector, float carrierPeak, DetectorLoad& load) {
  // A charged AVC capacitor draws no DC: solve the working point on the DC
  // load, then let the AVC branch start from that equilibrium.
  const DetectorLoad dc{load.conductance - load.avcConductance,
                        load.historyCurrent + load.avcConductance * detector.avcEnv, 0};
  detector.filteredStorageNode = static_cast<float>(
      equilibrium(detector.audioDiode, carrierPeak, 0, detector.audioChargeResistanceOhms,
                  dc, detector.filterResistanceOhms));
  detector.detectorStorageNode =
      detector.filteredStorageNode +
      detector.filterResistanceOhms *
          (dc.conductance * detector.filteredStorageNode + dc.historyCurrent);
  detector.detectorNode = detector.filteredStorageNode;
  detector.avcEnv = std::max(
      (detector.avcFromIfEnvelope ? detector.avcRectifierPerIfVolt * carrierPeak
                                  : detector.filteredStorageNode) -
          detector.avcDelayBiasVolts,
      0.0f);
  load.historyCurrent = dc.historyCurrent - load.avcConductance * detector.avcEnv;
  detector.warmStartPending = false;
}

float stepDetectorAudioNode(AMDetector& detector, float dt, float sourceVolts,
                            DetectorLoad load) {
  const uint64_t startNs = detector.metricsEnabled ? monotonicNowNs() : 0;
  int iterations = 0;
  const double cap1G = detector.detectorStorageCapFarads / double(dt);
  const double cap2G = detector.filterCapFarads / double(dt);
  const double outG = cap2G + load.conductance;
  const double outHistory = cap2G * detector.filteredStorageNode - load.historyCurrent;
  // Eliminate the post-detector RC node, retaining its capacitor history and
  // the exact volume-network load. R=0 is an exact wire, without a tiny shunt.
  const double denominator = 1 + detector.filterResistanceOhms * outG;
  const double totalG = cap1G + outG / denominator;
  const double unloaded =
      (cap1G * detector.detectorStorageNode + outHistory / denominator) / totalG;
  const double current = detector.audioDiode.current(
      sourceVolts - unloaded, detector.audioChargeResistanceOhms + 1 / totalG,
      &iterations);
  detector.detectorStorageNode = static_cast<float>(unloaded + current / totalG);
  const float node = static_cast<float>(
      (detector.detectorStorageNode + detector.filterResistanceOhms * outHistory) /
      denominator);
  ++detector.storageSolveCallCount;
  detector.storageSolveIterationCount += iterations;
  detector.storageSolveMaxIterations =
      std::max(detector.storageSolveMaxIterations, static_cast<uint32_t>(iterations));
  if (detector.metricsEnabled) detector.storageSolveTimeNs += monotonicNowNs() - startNs;
  detector.filteredStorageNode = node;
  return node;
}

}  // namespace am_detector_internal
