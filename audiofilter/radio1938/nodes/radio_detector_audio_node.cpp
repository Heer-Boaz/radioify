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
  // This pole is residual cleanup on the already-demodulated audio branch. It
  // should sit above the intended sideband so the detector audio node, not an
  // arbitrary monitor filter, sets the audible bandwidth.
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

float deriveDetectorAudioCapFarads(const Radio1938& radio, float loadG) {
  float safeLoadG = std::max(loadG, 1e-9f);
  float targetAudioPoleHz =
      std::clamp(1.15f * std::max(detectorAudioBandwidthHz(radio), 1.0f),
                 3000.0f, std::min(9000.0f, 0.18f * radio.sampleRate));
  return safeLoadG / (kRadioTwoPi * targetAudioPoleHz);
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
  float audioCapG = deriveDetectorAudioCapFarads(radio, loadG) / dt;
  float sourceG = 0.0f;
  if (audioRect > detectorAudio.audioNode) {
    sourceG = 1.0f / std::max(detector.audioChargeResistanceOhms, 1e-6f);
  }
  float denom = audioCapG + loadG + sourceG;
  assert(std::isfinite(denom) && denom > 1e-12f);
  detectorAudio.audioNode = std::max(
      (audioCapG * detectorAudio.audioNode + sourceG * audioRect) / denom, 0.0f);
  detectorAudio.audioEnv = detectorAudio.postLp.process(detectorAudio.audioNode);
  return detectorAudio.audioEnv;
}
