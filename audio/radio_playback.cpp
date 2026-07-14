#include "audioplayback_internal.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>

#include "pipeline_transition.h"

namespace {

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
  rebuildRadioFromTemplate(&state->radio1938, gAudio.radio1938Template,
                           static_cast<float>(gAudio.sampleRate), gAudio.lpHz,
                           gAudio.noise);
  state->radioPreview.initialize(state->radio1938,
                                 state->radioAmIngress,
                                 state->radioPreviewConfig,
                                 static_cast<float>(gAudio.sampleRate));
}

void configureRadioPlaybackTemplate() {
  gAudio.radio1938Template.applyPreset(Radio1938::Preset::Philco37116X);
  applyRadioSettingsToTemplate(gAudio.radio1938Template,
                               gAudio.radioPresetName,
                               gAudio.radioSettingsPath);
}

void prepareRadioPlaybackSource(AudioState* state) {
  if (!state) return;
  const uint32_t channels = std::max(1u, state->channels);
  state->radioBypass.reset(
      state->useRadio1938.load(std::memory_order_relaxed), state->sampleRate);
  state->radioDryScratch.resize(
      static_cast<size_t>(kRadioBypassScratchFrames) * channels);
}

void audioPlaybackProcessRadioBlock(AudioState* state,
                                    float* samples,
                                    uint32_t frames) {
  if (!state || !samples || frames == 0) return;
  const bool radioEnabled =
      state->useRadio1938.load(std::memory_order_relaxed);
  if (!state->radioBypass.needsWetSignal(radioEnabled)) {
    audioPipelineTransitionClearInputFadeIn(state->pipelineTransition);
    return;
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
