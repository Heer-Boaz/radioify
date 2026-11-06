#include "../../../radio.h"
#include "../../math/signal_math.h"
#include "am_detector_internal.h"
#include "receiver_input_network.h"

#include <algorithm>
#include <cmath>

namespace {

am_detector_internal::DetectorLoad detectorLoad(const Radio1938& radio) {
  const float dischargeG = radio.demod.am.audioDischargeResistanceOhms > 0
                               ? 1.0f / radio.demod.am.audioDischargeResistanceOhms
                               : 0;
  const auto& receiver = radio.receiverCircuit;
  // The passive input network advances once per audio frame, driven by the
  // detector's interval-average voltage. Its affine companion also loads every
  // IF substep: averaging G*V + I_history gives exactly the current committed by
  // ReceiverInputNetwork. A freshly started receiver begins at DC equilibrium.
  const auto port =
      receiver.warmStartPending
          ? receiverInputNetworkDc(receiver)
          : receiverInputNetworkStep(receiver, 1 / std::max<double>(radio.sampleRate, 1));
  am_detector_internal::DetectorLoad load{
      dischargeG + static_cast<float>(port.inputConductance),
      static_cast<float>(port.historyCurrent), 0};
  // Simple AVC: the filter resistor hangs on the detector load, so at audio
  // frequencies it is in parallel with it (its capacitor is an AC short)
  // while drawing no DC. That lowers the AC/DC load ratio and clips the
  // troughs of deep modulation. A separate AVC rectifier does not load here.
  const auto& am = radio.demod.am;
  if (!am.avcFromIfEnvelope) {
    load.avcConductance = 1.0f / am.avcFilterResistanceOhms;
    load.conductance += load.avcConductance;
    load.historyCurrent -= load.avcConductance * am.avcEnv;
  }
  return load;
}

float detectorWaveformCarrierHz(const AMDetector& detector, const Radio1938& radio) {
  if (!radio.ifStrip.enabled) return 0.0f;
  return std::fabs(radio.ifStrip.ifCenterHz + detector.tuneOffsetHz);
}

}  // namespace

void AMDetector::init(float newFs, float newBw, float newTuneHz) {
  fs = newFs;
  bwHz = newBw;
  tuneOffsetHz = newTuneHz;
  const double avcTau = double(avcFilterResistanceOhms) * avcFilterCapFarads;
  avcTick = avcTau > 0 ? static_cast<float>(1 - std::exp(-1 / (std::max(fs, 1.0f) * avcTau)))
                       : 1.0f;
  setBandwidth(newBw, newTuneHz);
  reset();
}

void AMDetector::setBandwidth(float newBw, float newTuneHz) {
  bwHz = newBw;
  tuneOffsetHz = newTuneHz;
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
  // Detector samples are returned to the source-envelope frame by IFStrip.
  // Shift discriminator probes into that frame as well. The high-side mixer
  // reverses spectral orientation, hence the negative tuning offset.
  afcLowOffsetHz = lowSenseHz - ifCenter - tuneOffsetHz;
  afcHighOffsetHz = highSenseHz - ifCenter - tuneOffsetHz;
  afcLowStep = kRadioTwoPi * (afcLowOffsetHz / std::max(fs, 1.0f));
  afcHighStep = kRadioTwoPi * (afcHighOffsetHz / std::max(fs, 1.0f));
  afcLowStepCos = std::cos(afcLowStep);
  afcLowStepSin = std::sin(afcLowStep);
  afcHighStepCos = std::cos(afcHighStep);
  afcHighStepSin = std::sin(afcHighStep);
  afcLowOscCos = std::cos(afcLowPhase);
  afcLowOscSin = std::sin(afcLowPhase);
  afcHighOscCos = std::cos(afcHighPhase);
  afcHighOscSin = std::sin(afcHighPhase);
  // Broad discriminator slopes must include the carrier at station centre.
  // afcSenseLpHz is the DC error smoothing, not a 34 Hz IF resonator width.
  afcLowProbe.setLowpass(fs, afcOffset, kRadioBiquadQ);
  afcHighProbe.setLowpass(fs, afcOffset, kRadioBiquadQ);
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
  detectorStorageNode = 0.0f;
  filteredStorageNode = 0.0f;
  detectorNode = 0.0f;
  avcEnv = 0.0f;
  ifWavePhase = 0.0f;
  prevPrevIfI = 0.0f;
  prevPrevIfQ = 0.0f;
  prevIfI = 0.0f;
  prevIfQ = 0.0f;
  ifEnvelopeHistorySamples = 0;
  lastWaveformSubsteps = 0;
  waveformSampleCount = 0;
  waveformIntervalCount = 0;
  waveformSplitIntervalCount = 0;
  waveformSolveStepCount = 0;
  storageSolveCallCount = 0;
  storageSolveIterationCount = 0;
  storageSolveMaxIterations = 0;
  afcProbeTimeNs = 0;
  detectorIslandTimeNs = 0;
  storageSolveTimeNs = 0;
  metricsEnabled = false;
  warmStartPending = true;
  afcError = 0.0f;
  afcLowPhase = 0.0f;
  afcHighPhase = 0.0f;
  afcLowOscCos = 1.0f;
  afcLowOscSin = 0.0f;
  afcHighOscCos = 1.0f;
  afcHighOscSin = 0.0f;
  afcLowProbe.reset();
  afcHighProbe.reset();
  afcErrorLp.reset();
}

float AMDetector::run(float signalI, float signalQ, Radio1938& radio) {
  metricsEnabled = radio.calibration.enabled;
  const float ifI = signalI;
  const float ifQ = signalQ;

  auto processProbe = [&](float step, float stepCos, float stepSin, float& oscCos,
                          float& oscSin, float& phase, IQBiquad& probe) {
    float mixedI = ifI * oscCos + ifQ * oscSin;
    float mixedQ = ifQ * oscCos - ifI * oscSin;
    auto filtered = probe.process(mixedI, mixedQ);
    phase = wrapPhase(phase + step);
    advanceNormalizedOscillator(stepCos, stepSin, oscCos, oscSin);
    return std::sqrt(filtered[0] * filtered[0] + filtered[1] * filtered[1]);
  };

  uint64_t afcStartNs = metricsEnabled ? am_detector_internal::monotonicNowNs() : 0;
  float afcLow = processProbe(afcLowStep, afcLowStepCos, afcLowStepSin, afcLowOscCos,
                              afcLowOscSin, afcLowPhase, afcLowProbe);
  float afcHigh = processProbe(afcHighStep, afcHighStepCos, afcHighStepSin, afcHighOscCos,
                               afcHighOscSin, afcHighPhase, afcHighProbe);
  if (metricsEnabled) {
    afcProbeTimeNs += am_detector_internal::monotonicNowNs() - afcStartNs;
  }

  float afcDen = std::max(afcLow + afcHigh, 1e-6f);
  float rawAfcError = (afcHigh - afcLow) / afcDen;
  afcError = afcErrorLp.process(rawAfcError);

  const auto load = detectorLoad(radio);
  float carrierHz = detectorWaveformCarrierHz(*this, radio);
  uint64_t islandStartNs = metricsEnabled ? am_detector_internal::monotonicNowNs() : 0;
  if (carrierHz > 0.0f) {
    am_detector_internal::runWaveformDetectorIsland(*this, ifI, ifQ, load, carrierHz);
  } else {
    am_detector_internal::runEnvelopeDetectorPath(*this, std::sqrt(ifI * ifI + ifQ * ifQ),
                                                  load);
    am_detector_internal::updateDetectorEnvelopeHistory(*this, ifI, ifQ);
  }
  if (metricsEnabled) {
    detectorIslandTimeNs += am_detector_internal::monotonicNowNs() - islandStartNs;
  }
  const float avcSource = avcFromIfEnvelope ? std::sqrt(ifI * ifI + ifQ * ifQ) : detectorNode;
  avcEnv += avcTick * (std::max(avcSource - avcDelayBiasVolts, 0.0f) - avcEnv);
  assert(std::isfinite(detectorNode) && std::isfinite(avcEnv));
  if (radio.calibration.enabled) {
    radio.calibration.detectorNodeVolts.accumulate(detectorNode);
  }
  return audioRect;
}
