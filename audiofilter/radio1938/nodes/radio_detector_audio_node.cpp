#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

void prechargeUnityLowpass(Biquad& biquad, float dcLevel) {
  biquad.z1 = dcLevel * (1.0f - biquad.b0);
  biquad.z2 = dcLevel * (biquad.b2 - biquad.a2);
}

uint32_t detectorAudioConfigRevision(const Radio1938& radio) {
  return radio.ifStrip.enabled ? radio.ifStrip.appliedConfigRevision
                               : radio.tuning.configRevision;
}

float detectorAudioBandwidthHz(const Radio1938& radio) {
  return radio.ifStrip.enabled ? radio.ifStrip.demodBandwidthHz
                               : radio.tuning.tunedBw;
}

void ensureDetectorAudioConfigured(Radio1938& radio) {
  auto& detectorAudio = radio.detectorAudio;
  uint32_t desiredRevision = detectorAudioConfigRevision(radio);
  if (detectorAudio.appliedConfigRevision == desiredRevision) return;

  float safeBwHz = std::max(detectorAudioBandwidthHz(radio), 1.0f);
  // This pole is residual cleanup on the reduced-order second-detector audio
  // branch. Keep it above the intended service sideband so the named detector
  // / receiver network, not a hidden monitor filter, sets the bandwidth.
  float audioCleanupHz = std::clamp(1.35f * safeBwHz, 3800.0f,
                                    std::min(9500.0f, 0.20f * radio.sampleRate));
  detectorAudio.postLp.setLowpass(radio.sampleRate, audioCleanupHz,
                                  kRadioBiquadQ);
  detectorAudio.appliedConfigRevision = desiredRevision;
}

float effectiveDetectorAudioLoadConductance(const Radio1938& radio) {
  float dischargeG =
      1.0f / std::max(radio.demod.am.audioDischargeResistanceOhms, 1e-6f);
  if (!radio.receiverCircuit.enabled) return dischargeG;
  return dischargeG + radio.receiverCircuit.detectorLoadConductance;
}

float detectorAudioStorageCapFarads(const Radio1938& radio) {
  // Keep the fast detector-audio branch anchored to the explicit detector
  // storage capacitor instead of inventing a separate audio pole from the
  // desired bandwidth. The branch is still reduced-order, but its state now
  // names the actual capacitance that stores charge at the detector.
  return std::max(radio.demod.am.detectorStorageCapFarads, 1e-12f);
}

}  // namespace

void RadioDetectorAudioNode::init(Radio1938& radio, RadioInitContext&) {
  radio.detectorAudio.appliedConfigRevision = 0;
  ensureDetectorAudioConfigured(radio);
}

void RadioDetectorAudioNode::reset(Radio1938& radio) {
  auto& detectorAudio = radio.detectorAudio;
  detectorAudio.audioNode = 0.0f;
  detectorAudio.audioEnv = 0.0f;
  detectorAudio.warmStartPending = true;
  detectorAudio.postLp.reset();
  detectorAudio.appliedConfigRevision = 0;
}

float RadioDetectorAudioNode::run(Radio1938& radio,
                                  float y,
                                  RadioSampleContext&) {
  ensureDetectorAudioConfigured(radio);
  auto& detectorAudio = radio.detectorAudio;
  const auto& detector = radio.demod.am;
  float audioRect = std::max(y, 0.0f);
  if (detectorAudio.warmStartPending) {
    detectorAudio.audioNode = audioRect;
    detectorAudio.audioEnv = audioRect;
    prechargeUnityLowpass(detectorAudio.postLp, audioRect);
    detectorAudio.warmStartPending = false;
    return detectorAudio.audioEnv;
  }

  float dt = 1.0f / std::max(radio.sampleRate, 1.0f);
  float loadG = effectiveDetectorAudioLoadConductance(radio);
  float audioCapG = detectorAudioStorageCapFarads(radio) / dt;
  float sourceG = 0.0f;
  if (audioRect > detectorAudio.audioNode) {
    sourceG = 1.0f / std::max(detector.audioChargeResistanceOhms, 1e-6f);
  }
  float denom = audioCapG + loadG + sourceG;
  assert(std::isfinite(denom) && denom > 1e-12f);
  detectorAudio.audioNode = std::max(
      (audioCapG * detectorAudio.audioNode + sourceG * audioRect) / denom, 0.0f);
  detectorAudio.audioEnv =
      std::max(detectorAudio.postLp.process(detectorAudio.audioNode), 0.0f);
  return detectorAudio.audioEnv;
}
