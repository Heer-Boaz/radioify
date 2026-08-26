#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "playback/video/analysis/scene_analysis.h"
#include "playback/video/edit/timeline.h"
#include "playback/video/edit/view.h"

namespace playback_video_edit {

SceneSuggestionSnapshot projectSceneSuggestion(
    const playback_video_analysis::SceneSuggestion& suggestion,
    const Timeline& timeline, bool selected);

bool sceneSuggestionVisibleOnTimeline(
    const playback_video_analysis::SceneSuggestion& suggestion,
    const Timeline& timeline);

std::optional<uint64_t> sceneSuggestionAtTimeline(
    const std::vector<SceneSuggestionSnapshot>& suggestions,
    int64_t timelineUs, int64_t toleranceUs);

}  // namespace playback_video_edit
