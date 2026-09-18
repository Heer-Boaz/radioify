#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "playback/video/analysis/edit_review.h"
#include "playback/video/edit/timeline.h"
#include "playback/video/edit/view.h"

namespace playback_video_edit {

SceneSuggestionKind projectSceneSuggestionKind(
    playback_video_analysis::EditDisposition kind);
bool sceneSuggestionMatchesFilter(SceneSuggestionKind kind,
                                  SceneSuggestionFilter filter);
SceneSuggestionFilter nextSceneSuggestionFilter(
    SceneSuggestionFilter filter);
const char* sceneSuggestionFilterLabel(SceneSuggestionFilter filter);
const char* sceneSuggestionKindLabel(SceneSuggestionKind kind);

SceneSuggestionSnapshot projectSceneSuggestion(
    const playback_video_analysis::EditProposal& suggestion,
    const Timeline& timeline, bool selected);

bool sceneSuggestionVisibleOnTimeline(
    const playback_video_analysis::EditProposal& suggestion,
    const Timeline& timeline);

std::optional<uint64_t> sceneSuggestionAtTimeline(
    const std::vector<SceneSuggestionSnapshot>& suggestions,
    int64_t timelineUs, int64_t toleranceUs);

}  // namespace playback_video_edit
