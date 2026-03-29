#include "../../../radio.h"
#include "../../math/signal_math.h"
#include "am_detector_internal.h"

#include <algorithm>
#include <cmath>

namespace {

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

void advanceProbeOscillator(float stepCos,
                            float stepSin,
                            float& oscCos,
                            float& oscSin) {
  float nextCos = oscCos * stepCos - oscSin * stepSin;
  float nextSin = oscSin * stepCos + oscCos * stepSin;
  float magnitudeSq = nextCos * nextCos + nextSin * nextSin;
  float renorm = 1.5f - 0.5f * magnitudeSq;
  oscCos = nextCos * renorm;
  oscSin = nextSin * renorm;
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
  afcLowStepCos = std::cos(afcLowStep);
  afcLowStepSin = std::sin(afcLowStep);
  afcHighStepCos = std::cos(afcHighStep);
  afcHighStepSin = std::sin(afcHighStep);
  afcLowOscCos = std::cos(afcLowPhase);
  afcLowOscSin = std::sin(afcLowPhase);
  afcHighOscCos = std::cos(afcHighPhase);
  afcHighOscSin = std::sin(afcHighPhase);
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
  ifCrackleEnv = 0.0f;
  ifCrackleDecay = 0.0f;
  ifCracklePhase = 0.0f;
  ifCrackleEventCount = 0;
  ifCrackleMaxBurstAmp = 0.0f;
  ifCrackleMaxEnv = 0.0f;
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

float AMDetector::run(float signalI,
                      float signalQ,
                      float ifNoiseAmp,
                      Radio1938& radio,
                      float ifCrackleAmp,
                      float ifCrackleRate) {
  constexpr float kInvSqrt2 = 0.70710678118f;
  metricsEnabled = radio.calibration.enabled;
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

  auto processProbe = [&](float step,
                          float stepCos,
                          float stepSin,
                          float& oscCos,
                          float& oscSin,
                          float& phase,
                          IQBiquad& probe) {
    float mixedI = ifI * oscCos + ifQ * oscSin;
    float mixedQ = ifQ * oscCos - ifI * oscSin;
    auto filtered = probe.process(mixedI, mixedQ);
    phase = wrapPhase(phase + step);
    advanceProbeOscillator(stepCos, stepSin, oscCos, oscSin);
    return std::sqrt(filtered[0] * filtered[0] + filtered[1] * filtered[1]);
  };

  uint64_t afcStartNs =
      metricsEnabled ? am_detector_internal::monotonicNowNs() : 0;
  float afcLow = processProbe(afcLowStep, afcLowStepCos, afcLowStepSin,
                              afcLowOscCos, afcLowOscSin, afcLowPhase,
                              afcLowProbe);
  float afcHigh = processProbe(afcHighStep, afcHighStepCos, afcHighStepSin,
                               afcHighOscCos, afcHighOscSin, afcHighPhase,
                               afcHighProbe);
  if (metricsEnabled) {
    afcProbeTimeNs += am_detector_internal::monotonicNowNs() - afcStartNs;
  }

  float afcDen = std::max(afcLow + afcHigh, 1e-6f);
  float rawAfcError = (afcHigh - afcLow) / afcDen;
  afcError = afcErrorLp.process(rawAfcError);

  float detectorLeakG = effectiveDetectorLoadConductance(radio);
  float carrierHz = detectorWaveformCarrierHz(*this, radio);
  uint64_t islandStartNs =
      metricsEnabled ? am_detector_internal::monotonicNowNs() : 0;
  if (carrierHz > 0.0f) {
    am_detector_internal::runWaveformDetectorIsland(*this, ifI, ifQ,
                                                    detectorLeakG, carrierHz);
  } else {
    am_detector_internal::runEnvelopeDetectorPath(
        *this, std::sqrt(ifI * ifI + ifQ * ifQ), detectorLeakG);
    am_detector_internal::updateDetectorEnvelopeHistory(*this, ifI, ifQ);
  }
  if (metricsEnabled) {
    detectorIslandTimeNs +=
        am_detector_internal::monotonicNowNs() - islandStartNs;
  }
  assert(std::isfinite(detectorNode) && std::isfinite(avcEnv));
  if (radio.calibration.enabled) {
    radio.calibration.detectorNodeVolts.accumulate(detectorNode);
  }
  return audioRect;
}
