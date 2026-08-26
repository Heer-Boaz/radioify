#include "playback/video/edit/scene_suggestions.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace playback_video_edit {
SceneSuggestionKind projectSceneSuggestionKind(
    playback_video_analysis::SceneKind kind) {
  switch (kind) {
    case playback_video_analysis::SceneKind::Gameplay:
      return SceneSuggestionKind::Gameplay;
    case playback_video_analysis::SceneKind::Dialogue:
      return SceneSuggestionKind::Dialogue;
    case playback_video_analysis::SceneKind::Cutscene:
      return SceneSuggestionKind::Cutscene;
    case playback_video_analysis::SceneKind::MenuOrLoading:
      return SceneSuggestionKind::MenuOrLoading;
  }
  return SceneSuggestionKind::Gameplay;
}

bool sceneSuggestionMatchesFilter(SceneSuggestionKind kind,
                                  SceneSuggestionFilter filter) {
  switch (filter) {
    case SceneSuggestionFilter::All:
      return true;
    case SceneSuggestionFilter::Cutscenes:
      return kind == SceneSuggestionKind::Cutscene;
    case SceneSuggestionFilter::Dialogue:
      return kind == SceneSuggestionKind::Dialogue;
    case SceneSuggestionFilter::Gameplay:
      return kind == SceneSuggestionKind::Gameplay;
    case SceneSuggestionFilter::MenuOrLoading:
      return kind == SceneSuggestionKind::MenuOrLoading;
  }
  return true;
}

SceneSuggestionFilter nextSceneSuggestionFilter(
    SceneSuggestionFilter filter) {
  switch (filter) {
    case SceneSuggestionFilter::All:
      return SceneSuggestionFilter::Cutscenes;
    case SceneSuggestionFilter::Cutscenes:
      return SceneSuggestionFilter::Dialogue;
    case SceneSuggestionFilter::Dialogue:
      return SceneSuggestionFilter::Gameplay;
    case SceneSuggestionFilter::Gameplay:
      return SceneSuggestionFilter::MenuOrLoading;
    case SceneSuggestionFilter::MenuOrLoading:
      return SceneSuggestionFilter::All;
  }
  return SceneSuggestionFilter::All;
}

const char* sceneSuggestionFilterLabel(SceneSuggestionFilter filter) {
  switch (filter) {
    case SceneSuggestionFilter::All:
      return "All";
    case SceneSuggestionFilter::Cutscenes:
      return "Cutscenes";
    case SceneSuggestionFilter::Dialogue:
      return "Dialogue";
    case SceneSuggestionFilter::Gameplay:
      return "Gameplay";
    case SceneSuggestionFilter::MenuOrLoading:
      return "Menu/loading";
  }
  return "All";
}

const char* sceneSuggestionKindLabel(SceneSuggestionKind kind) {
  switch (kind) {
    case SceneSuggestionKind::Gameplay:
      return "Gameplay segment";
    case SceneSuggestionKind::Dialogue:
      return "Dialogue segment";
    case SceneSuggestionKind::Cutscene:
      return "Cutscene candidate";
    case SceneSuggestionKind::MenuOrLoading:
      return "Menu/loading segment";
  }
  return "Detected segment";
}

const char* sceneSuggestionStrengthLabel(float confidence) {
  return confidence >= 0.72f ? "Strong" : "Possible";
}

bool sceneSuggestionVisibleOnTimeline(
    const playback_video_analysis::SceneSuggestion& suggestion,
    const Timeline& timeline) {
  return std::any_of(
      timeline.keptRanges().begin(), timeline.keptRanges().end(),
      [&](const SourceRange& kept) {
        return std::min(kept.endUs, suggestion.endUs) >
               std::max(kept.startUs, suggestion.startUs);
      });
}

SceneSuggestionSnapshot projectSceneSuggestion(
    const playback_video_analysis::SceneSuggestion& suggestion,
    const Timeline& timeline, bool selected) {
  SceneSuggestionSnapshot out;
  out.id = suggestion.id;
  out.source = {suggestion.startUs, suggestion.endUs};
  out.kind = projectSceneSuggestionKind(suggestion.kind);
  out.confidence = suggestion.confidence;
  out.selected = selected;

  int64_t timelineCursorUs = 0;
  for (const SourceRange& kept : timeline.keptRanges()) {
    const int64_t overlapStartUs =
        std::max(kept.startUs, suggestion.startUs);
    const int64_t overlapEndUs = std::min(kept.endUs, suggestion.endUs);
    if (overlapEndUs > overlapStartUs) {
      const int64_t startUs =
          timelineCursorUs + (overlapStartUs - kept.startUs);
      out.spans.push_back(
          {startUs, startUs + (overlapEndUs - overlapStartUs)});
    }
    timelineCursorUs += kept.durationUs();
  }
  return out;
}

std::optional<uint64_t> sceneSuggestionAtTimeline(
    const std::vector<SceneSuggestionSnapshot>& suggestions,
    int64_t timelineUs, int64_t toleranceUs) {
  toleranceUs = std::max<int64_t>(0, toleranceUs);

  // Timeline spans are half-open. Resolve containment across every suggestion
  // before considering nearby edges, so a shared boundary belongs to the span
  // that starts there rather than to the preceding span's exclusive end.
  for (const SceneSuggestionSnapshot& suggestion : suggestions) {
    for (const SceneSuggestionSpanSnapshot& span : suggestion.spans) {
      if (span.timelineEndUs > span.timelineStartUs &&
          timelineUs >= span.timelineStartUs &&
          timelineUs < span.timelineEndUs) {
        return suggestion.id;
      }
    }
  }

  const SceneSuggestionSnapshot* best = nullptr;
  int64_t bestDistance = (std::numeric_limits<int64_t>::max)();
  for (const SceneSuggestionSnapshot& suggestion : suggestions) {
    for (const SceneSuggestionSpanSnapshot& span : suggestion.spans) {
      if (span.timelineEndUs <= span.timelineStartUs) continue;
      int64_t distance = 0;
      if (timelineUs < span.timelineStartUs) {
        distance = span.timelineStartUs - timelineUs;
      } else {
        distance = timelineUs - span.timelineEndUs;
      }
      if (distance > toleranceUs || distance >= bestDistance) continue;
      best = &suggestion;
      bestDistance = distance;
    }
  }
  return best ? std::optional<uint64_t>(best->id) : std::nullopt;
}

}  // namespace playback_video_edit
