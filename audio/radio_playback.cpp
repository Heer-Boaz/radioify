#include "radio_playback.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "pipeline_transition.h"

namespace {

constexpr uint32_t kRadioProcessChannels = 1u;
constexpr uint32_t kRadioTransitionScratchFrames = 2048u;

}  // namespace

void RadioPlaybackFilter::initialize(
    const RadioPlaybackFilterConfig& config) {
  assert(sampleRate_ == 0 && config.sampleRate > 0 &&
         config.outputChannels > 0);
  sampleRate_ = config.sampleRate;
  ingress_.reception = config.reception;
  initializeReceiver(typical_, RadioReceiverProfile::Typical1930s, config);
  initializeReceiver(philco_, RadioReceiverProfile::Philco37116, config);
  requestedMode_.store(config.initialMode, std::memory_order_relaxed);
  resetGeneration_.store(0, std::memory_order_relaxed);
  appliedResetGeneration_ = 0;
  resetSource(config.outputChannels);
}

void RadioPlaybackFilter::initializeReceiver(
    Receiver& receiver,
    RadioReceiverProfile profile,
    const RadioPlaybackFilterConfig& config) {
  receiver.radio.applyReceiverProfile(profile);
  if (!config.settingsPath.empty()) {
    std::string error;
    if (!applyRadioSettingsIni(receiver.radio, config.settingsPath,
                               config.presetName, &error)) {
      std::fprintf(stderr,
                   "WARNING: Failed to apply radio settings from %s: %s\n",
                   config.settingsPath.c_str(), error.c_str());
    }
  }
  receiver.radio.init(kRadioProcessChannels,
                      static_cast<float>(config.sampleRate),
                      config.bandwidthHz, config.noise);
  receiver.preview.initialize(receiver.radio, ingress_, previewConfig_,
                              static_cast<float>(config.sampleRate));
  receiver.preview.reserveBlockFrames(kRadioTransitionScratchFrames);
}

void RadioPlaybackFilter::resetSource(uint32_t outputChannels) {
  assert(sampleRate_ > 0 && outputChannels > 0);
  typical_.radio.reset();
  typical_.preview.reset();
  philco_.radio.reset();
  philco_.preview.reset();
  transition_.reset(requestedMode_.load(std::memory_order_acquire),
                    sampleRate_);
  appliedResetGeneration_ =
      resetGeneration_.load(std::memory_order_acquire);
  dryScratch_.resize(static_cast<size_t>(kRadioTransitionScratchFrames) *
                     outputChannels);
}

void RadioPlaybackFilter::requestReset() {
  resetGeneration_.fetch_add(1, std::memory_order_release);
}

RadioFilterMode RadioPlaybackFilter::mode() const {
  return requestedMode_.load(std::memory_order_acquire);
}

void RadioPlaybackFilter::cycleMode() {
  RadioFilterMode current = requestedMode_.load(std::memory_order_relaxed);
  while (!requestedMode_.compare_exchange_weak(
      current, nextRadioFilterMode(current), std::memory_order_release,
      std::memory_order_relaxed)) {
  }
}

RadioPlaybackFilter::Receiver& RadioPlaybackFilter::receiverFor(
    RadioFilterMode mode) {
  switch (mode) {
    case RadioFilterMode::Typical1930s:
      return typical_;
    case RadioFilterMode::Philco37116:
      return philco_;
    case RadioFilterMode::Off:
      std::abort();
  }
  std::abort();
}

void RadioPlaybackFilter::resetReceiver(RadioFilterMode mode) {
  Receiver& receiver = receiverFor(mode);
  receiver.radio.reset();
  receiver.preview.reset();
}

bool RadioPlaybackFilter::process(
    float* samples,
    uint32_t frames,
    uint32_t channels,
    AudioPipelineTransition& pipelineTransition) {
  assert(sampleRate_ > 0 && samples && frames > 0 && channels > 0);
  bool receiverChanged = transition_.retarget(mode());
  receiverChanged = transition_.commitAtDryBoundary() || receiverChanged;

  const uint64_t resetGeneration =
      resetGeneration_.load(std::memory_order_acquire);
  if (!transition_.needsWetSignal()) {
    appliedResetGeneration_ = resetGeneration;
    audioPipelineTransitionClearInputFadeIn(pipelineTransition);
    return false;
  }
  if (receiverChanged || resetGeneration != appliedResetGeneration_) {
    resetReceiver(transition_.activeMode());
    appliedResetGeneration_ = resetGeneration;
  }

  bool clipped = false;
  uint32_t frameOffset = 0;
  while (frameOffset < frames) {
    if (transition_.commitAtDryBoundary()) {
      resetReceiver(transition_.activeMode());
    }
    if (!transition_.needsWetSignal()) {
      audioPipelineTransitionClearInputFadeIn(pipelineTransition);
      break;
    }

    const bool blending = transition_.blending();
    const uint32_t boundaryFrames =
        transition_.framesUntilBoundary(sampleRate_);
    const uint32_t blockFrames =
        blending ? std::min({kRadioTransitionScratchFrames, boundaryFrames,
                             frames - frameOffset})
                 : std::min(kRadioTransitionScratchFrames,
                            frames - frameOffset);
    assert(blockFrames > 0);
    float* block = samples + static_cast<size_t>(frameOffset) * channels;
    audioPipelineTransitionApplyInputFadeIn(pipelineTransition, block,
                                            blockFrames, channels);

    if (blending) {
      const size_t blockSamples =
          static_cast<size_t>(blockFrames) * channels;
      assert(dryScratch_.size() >= blockSamples);
      std::memcpy(dryScratch_.data(), block, blockSamples * sizeof(float));
    }

    Receiver& receiver = receiverFor(transition_.activeMode());
    receiver.preview.runBlock(receiver.radio, block, blockFrames, channels);
    clipped = clipped || receiver.radio.diagnostics.outputClip;
    if (blending) {
      transition_.blend(block, dryScratch_.data(), blockFrames, channels,
                        sampleRate_);
    }
    frameOffset += blockFrames;
  }
  return clipped;
}
