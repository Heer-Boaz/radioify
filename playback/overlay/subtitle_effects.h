#pragma once

#include <cstdint>

#include "subtitle_manager.h"
#include "videowindow.h"

namespace playback_overlay {

float subtitleFadeOpacity(const SubtitleCue& cue, int64_t clockUs);
void applySubtitleTimelineEffects(WindowUiState::SubtitleCue* item,
                                  const SubtitleCue& cue, int64_t clockUs,
                                  float fadeOpacity);

}  // namespace playback_overlay
