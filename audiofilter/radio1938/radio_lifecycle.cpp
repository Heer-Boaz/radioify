#include "../../radio.h"
#include "../math/signal_math.h"
#include "audio_pipeline.h"
#include "radio_presets.h"

#include <algorithm>

namespace {

void applySetIdentity(Radio1938& radio) {
  auto& identity = radio.identity;
  float drift = std::clamp(identity.driftDepth, 0.0f, 0.25f);
  identity.frontEndAntennaDrift =
      std::clamp(applySeededDrift(1.0f, 0.45f * drift, identity.seed, 0x4101u),
                 0.78f, 1.22f);
  identity.frontEndRfDrift =
      std::clamp(applySeededDrift(1.0f, 0.40f * drift, identity.seed, 0x4102u),
                 0.80f, 1.20f);
  identity.mixerDriveDrift =
      std::clamp(applySeededDrift(1.0f, 0.28f * drift, identity.seed, 0x4103u),
                 0.86f, 1.14f);
  identity.mixerBiasDrift =
      std::clamp(applySeededDrift(1.0f, 0.18f * drift, identity.seed, 0x4104u),
                 0.90f, 1.10f);
  identity.ifPrimaryDrift =
      std::clamp(applySeededDrift(1.0f, 0.35f * drift, identity.seed, 0x4105u),
                 0.84f, 1.16f);
  identity.ifSecondaryDrift =
      std::clamp(applySeededDrift(1.0f, 0.35f * drift, identity.seed, 0x4106u),
                 0.84f, 1.16f);
  identity.ifCouplingDrift =
      std::clamp(applySeededDrift(1.0f, 0.40f * drift, identity.seed, 0x4107u),
                 0.82f, 1.18f);
  identity.detectorLoadDrift =
      std::clamp(applySeededDrift(1.0f, 0.30f * drift, identity.seed, 0x4108u),
                 0.88f, 1.12f);
}

void refreshIdentityDependentStages(Radio1938& radio) {
  applySetIdentity(radio);
  radio.tuning.configRevision++;
  RadioInitContext initCtx{};
  initCtx.tunedBw = radio.tuning.tunedBw;
  RadioFrontEndNode::init(radio, initCtx);
  RadioFrontEndNode::reset(radio);
  RadioMixerNode::init(radio, initCtx);
  RadioMixerNode::reset(radio);
  RadioIFStripNode::init(radio, initCtx);
  RadioIFStripNode::reset(radio);
  RadioAMDetectorNode::init(radio, initCtx);
  RadioAMDetectorNode::reset(radio);
  RadioDetectorAudioNode::init(radio, initCtx);
  RadioDetectorAudioNode::reset(radio);
  RadioReceiverCircuitNode::init(radio, initCtx);
  RadioReceiverCircuitNode::reset(radio);
  RadioPowerNode::init(radio, initCtx);
  RadioPowerNode::reset(radio);
  if (radio.calibration.enabled) {
    radio.resetCalibration();
  }
}

}  // namespace

std::string_view Radio1938::presetName(Preset preset) {
  switch (preset) {
    case Preset::Philco37116:
      return "philco_37_116";
  }
  return "philco_37_116";
}

Radio1938::Radio1938() {
  applyPreset(preset);
}

std::string_view Radio1938::passName(PassId id) {
  std::string_view name = radio1938AudioPipeline().passName(id);
  return name.empty() ? "Unknown" : name;
}

bool Radio1938::applyPreset(std::string_view presetNameValue) {
  if (presetNameValue == "philco_37_116") {
    applyPreset(Preset::Philco37116);
    return true;
  }
  if (presetNameValue == "philco_37_116x") {
    applyPreset(Preset::Philco37116);
    return true;
  }
  return false;
}

void Radio1938::applyPreset(Preset presetValue) {
  preset = presetValue;
  applyRadioPreset(*this, presetValue);
  if (!initialized) return;
  init(channels, sampleRate, bwHz, noiseWeight);
}

void Radio1938::setIdentitySeed(uint32_t seed) {
  identity.seed = seed;
  if (!initialized) return;
  refreshIdentityDependentStages(*this);
}

void Radio1938::setCalibrationEnabled(bool enabled) {
  calibration.enabled = enabled;
  resetCalibration();
}

void Radio1938::resetCalibration() {
  calibration.reset();
  demod.am.waveformSampleCount = 0;
  demod.am.waveformIntervalCount = 0;
  demod.am.waveformSplitIntervalCount = 0;
  demod.am.waveformSolveStepCount = 0;
  demod.am.storageSolveCallCount = 0;
  demod.am.storageSolveIterationCount = 0;
  demod.am.storageSolveMaxIterations = 0;
  demod.am.afcProbeTimeNs = 0;
  demod.am.detectorIslandTimeNs = 0;
  demod.am.storageSolveTimeNs = 0;
  demod.am.metricsEnabled = false;
  power.outputTransformerSubstepCount = 0;
  power.outputNewtonIterationCount = 0;
  power.outputNewtonMaxIterations = 0;
  power.interstageSubstepCount = 0;
  power.interstageIterationCount = 0;
  power.interstageMaxIterations = 0;
  power.interstageSolveTimeNs = 0;
  power.interstageDriverEvalCount = 0;
  power.interstageDriverEvalTimeNs = 0;
  power.outputSolveTimeNs = 0;
}

void Radio1938::init(int ch, float sr, float bw, float noise) {
  channels = ch;
  sampleRate = sr;
  bwHz = bw;
  noiseWeight = noise;
  applySetIdentity(*this);
  RadioInitContext initCtx{};
  lifecycle.initialize(*this, initCtx);
  graph.compile();
  initialized = true;
  if (calibration.enabled) {
    resetCalibration();
  }
  reset();
}

void Radio1938::reset() {
  diagnostics.reset();
  iqInput.resetRuntime();
  lifecycle.reset(*this);
  if (calibration.enabled) {
    resetCalibration();
  }
}
