#include "audioplayback_internal.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>

#include "pipeline_transition.h"

namespace {

size_t receiverTemplateIndex(RadioReceiverProfile profile) {
  switch (profile) {
    case RadioReceiverProfile::Typical1930s:
      return 0u;
    case RadioReceiverProfile::Philco37116:
      return 1u;
  }
  return 0u;
}

void rebuildRadioFromTemplate(Radio1938* target,
                              const Radio1938& source,
                              float sampleRate,
                              float bwHz,
                              float noise) {
  if (!target) return;
  *target = source;
  target->init(kRadioProcessChannels, sampleRate, bwHz, noise);
}

void applyRadioSettingsToTemplate(Radio1938& target,
                                  const std::string& presetName,
                                  const std::string& settingsPath) {
  if (settingsPath.empty()) return;
  std::string error;
  if (!applyRadioSettingsIni(target, settingsPath, presetName, &error)) {
    std::fprintf(stderr,
                 "WARNING: Failed to apply radio settings from %s: %s\n",
                 settingsPath.c_str(),
                 error.c_str());
  }
}

}  // namespace

void rebuildRadioPreviewChain(AudioState* state) {
  if (!state) return;
  const RadioReceiverProfile requestedProfile =
      radioFilterModeReceiverProfile(
          state->radioFilterMode.load(std::memory_order_relaxed));
  const Radio1938& sourceTemplate =
      gAudio.radio1938Templates[receiverTemplateIndex(requestedProfile)];
  rebuildRadioFromTemplate(&state->radio1938, sourceTemplate,
                           static_cast<float>(gAudio.sampleRate), gAudio.lpHz,
                           gAudio.noise);
  state->radioPreview.initialize(state->radio1938,
                                 state->radioAmIngress,
                                 state->radioPreviewConfig,
                                 static_cast<float>(gAudio.sampleRate));
  state->activeRadioReceiverProfile = requestedProfile;
}

void configureRadioPlaybackTemplates() {
  for (RadioReceiverProfile profile : {RadioReceiverProfile::Typical1930s,
                                       RadioReceiverProfile::Philco37116}) {
    Radio1938& target =
        gAudio.radio1938Templates[receiverTemplateIndex(profile)];
    target.applyReceiverProfile(profile);
    target.init(kRadioProcessChannels, static_cast<float>(gAudio.sampleRate),
                gAudio.lpHz, gAudio.noise);
    applyRadioSettingsToTemplate(target, gAudio.radioPresetName,
                                 gAudio.radioSettingsPath);
  }
}

void prepareRadioPlaybackSource(AudioState* state) {
  if (!state) return;
  const uint32_t channels = std::max(1u, state->channels);
  const bool radioEnabled = radioFilterModeEnabled(
      state->radioFilterMode.load(std::memory_order_relaxed));
  state->radioBypass.reset(radioEnabled, state->sampleRate);
  state->radioDryScratch.resize(
      static_cast<size_t>(kRadioBypassScratchFrames) * channels);
}

void audioPlaybackProcessRadioBlock(AudioState* state,
                                    float* samples,
                                    uint32_t frames) {
  if (!state || !samples || frames == 0) return;
  const RadioFilterMode requestedMode =
      state->radioFilterMode.load(std::memory_order_relaxed);
  const bool radioEnabled = radioFilterModeEnabled(requestedMode);
  const RadioReceiverProfile requestedProfile =
      radioFilterModeReceiverProfile(requestedMode);
  if (!radioEnabled) {
    state->radioBypass.cancelWetReplacement();
  } else if (requestedProfile != state->activeRadioReceiverProfile &&
             !state->radioBypass.wetReplacementActive()) {
    state->radioBypass.requestWetReplacement();
  }
  if (!state->radioBypass.needsWetSignal(radioEnabled)) {
    audioPipelineTransitionClearInputFadeIn(state->pipelineTransition);
    return;
  }

  if (state->radioBypass.wetReplacementReadyToCommit()) {
    rebuildRadioPreviewChain(state);
    state->radioBypass.commitWetReplacement();
  }
  const bool wetSignalStarted =
      state->radioBypass.activateWetSignal(radioEnabled);
  const bool resetRequested =
      state->radioResetPending.exchange(false, std::memory_order_relaxed);
  if (wetSignalStarted || resetRequested) {
    rebuildRadioPreviewChain(state);
  }

  const uint32_t channels = std::max(1u, state->channels);
  uint32_t frameOffset = 0;
  while (frameOffset < frames &&
         state->radioBypass.needsWetSignal(radioEnabled)) {
    if (state->radioBypass.wetReplacementReadyToCommit()) {
      rebuildRadioPreviewChain(state);
      state->radioBypass.commitWetReplacement();
    }
    const uint32_t blendFrames = state->radioBypass.blendFramesRemaining(
        radioEnabled, state->sampleRate);
    const bool needsDry = blendFrames > 0;
    const uint32_t blockFrames =
        needsDry ? std::min({kRadioBypassScratchFrames, blendFrames,
                             frames - frameOffset})
                 : frames - frameOffset;
    float* block = samples + static_cast<size_t>(frameOffset) * channels;
    audioPipelineTransitionApplyInputFadeIn(
        state->pipelineTransition, block, blockFrames, channels);

    if (needsDry) {
      const size_t blockSamples =
          static_cast<size_t>(blockFrames) * channels;
      assert(state->radioDryScratch.size() >= blockSamples);
      std::memcpy(state->radioDryScratch.data(), block,
                  blockSamples * sizeof(float));
    }

    state->radioPreview.runBlock(state->radio1938, block, blockFrames,
                                 channels);
    if (needsDry) {
      state->radioBypass.blend(
          block, state->radioDryScratch.data(), blockFrames, channels,
          radioEnabled, state->sampleRate);
    }
    frameOffset += blockFrames;
  }

  if (state->radio1938.diagnostics.anyClip) {
    audioPlaybackHoldClipAlert(state);
  }
}
