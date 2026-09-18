#include "playback/video/edit/scene_suggestions.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace playback_video_edit {
SceneSuggestionKind projectSceneSuggestionKind(
    playback_video_analysis::EditDisposition kind) {
  switch (kind) {
    case playback_video_analysis::EditDisposition::Keep:
      return SceneSuggestionKind::Keep;
    case playback_video_analysis::EditDisposition::Shorten:
      return SceneSuggestionKind::Shorten;
    case playback_video_analysis::EditDisposition::Review:
      return SceneSuggestionKind::Review;
  }
  return SceneSuggestionKind::Review;
}

bool sceneSuggestionMatchesFilter(SceneSuggestionKind kind,
                                  SceneSuggestionFilter filter) {
  switch (filter) {
    case SceneSuggestionFilter::All:
      return true;
    case SceneSuggestionFilter::Keep:
      return kind == SceneSuggestionKind::Keep;
    case SceneSuggestionFilter::Shorten:
      return kind == SceneSuggestionKind::Shorten;
    case SceneSuggestionFilter::Review:
      return kind == SceneSuggestionKind::Review;
  }
  return true;
}

SceneSuggestionFilter nextSceneSuggestionFilter(
    SceneSuggestionFilter filter) {
  switch (filter) {
    case SceneSuggestionFilter::All:
      return SceneSuggestionFilter::Keep;
    case SceneSuggestionFilter::Keep:
      return SceneSuggestionFilter::Shorten;
    case SceneSuggestionFilter::Shorten:
      return SceneSuggestionFilter::Review;
    case SceneSuggestionFilter::Review:
      return SceneSuggestionFilter::All;
  }
  return SceneSuggestionFilter::All;
}

const char* sceneSuggestionFilterLabel(SceneSuggestionFilter filter) {
  switch (filter) {
    case SceneSuggestionFilter::All:
      return "All";
    case SceneSuggestionFilter::Keep:
      return "Keep";
    case SceneSuggestionFilter::Shorten:
      return "Shorten";
    case SceneSuggestionFilter::Review:
      return "Review";
  }
  return "All";
}

const char* sceneSuggestionKindLabel(SceneSuggestionKind kind) {
  switch (kind) {
    case SceneSuggestionKind::Keep:
      return "Keep";
    case SceneSuggestionKind::Shorten:
      return "Shorten";
    case SceneSuggestionKind::Review:
      return "Review";
  }
  return "Review";
}

bool sceneSuggestionVisibleOnTimeline(
    const playback_video_analysis::EditProposal& suggestion,
    const Timeline& timeline) {
  return std::any_of(
      timeline.keptRanges().begin(), timeline.keptRanges().end(),
      [&](const SourceRange& kept) {
        return std::min(kept.endUs, suggestion.endUs) >
               std::max(kept.startUs, suggestion.startUs);
      });
}

SceneSuggestionSnapshot projectSceneSuggestion(
    const playback_video_analysis::EditProposal& suggestion,
    const Timeline& timeline, bool selected) {
  SceneSuggestionSnapshot out;
  out.id = suggestion.id;
  out.source = {suggestion.startUs, suggestion.endUs};
  out.kind = projectSceneSuggestionKind(suggestion.disposition);
  out.reason = suggestion.reason;
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
