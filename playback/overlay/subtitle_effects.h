#pragma once

#include <cstdint>

#include "playback/video/subtitle/manager.h"
#include "playback/video/framebuffer/window/window.h"

namespace playback_overlay {

float subtitleFadeOpacity(const SubtitleCue& cue, int64_t clockUs);
void applySubtitleTimelineEffects(WindowUiState::SubtitleCue* item,
                                  const SubtitleCue& cue, int64_t clockUs,
                                  float fadeOpacity);

}  // namespace playback_overlay
