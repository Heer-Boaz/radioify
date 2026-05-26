#include "audioplayback_internal.h"

#include <algorithm>
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
  state->radioPreviewConfig.programBandwidthHz = 0.48f * gAudio.lpHz;
  state->radioPreview.initialize(state->radio1938,
                                 state->radioAmIngress,
                                 state->radioPreviewConfig,
                                 static_cast<float>(gAudio.sampleRate));
}

void applyRadioTogglePreset() {
  gAudio.radio1938Template.applyPreset(Radio1938::Preset::Philco37116X);
  applyRadioSettingsToTemplate(gAudio.radio1938Template,
                               gAudio.radioPresetName,
                               gAudio.radioSettingsPath);
  rebuildRadioPreviewChain(&gAudio.state);
}

void audioPlaybackProcessRadioBlock(AudioState* state,
                                    float* samples,
                                    uint32_t frames) {
  if (!state || !samples || frames == 0) return;
  if (state->radioResetPending.exchange(false, std::memory_order_relaxed)) {
    rebuildRadioPreviewChain(state);
  }
  const uint32_t channels = std::max(1u, state->channels);
  audioPipelineTransitionApplyInputFadeIn(state->pipelineTransition, samples,
                                          frames, channels);
  state->radioPreview.runBlock(state->radio1938, samples, frames, channels);
  if (state->radio1938.diagnostics.anyClip) {
    audioPlaybackHoldClipAlert(state);
  }
}
