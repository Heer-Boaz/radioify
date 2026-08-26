#include "playback/video/edit/scene_suggestion_review.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "playback/video/edit/scene_suggestions.h"

namespace playback_video_edit {

void SceneSuggestionReview::beginAnalysis() {
  panelVisible_ = true;
  selectedId_.reset();
  hiddenIds_.clear();
  hiddenHistory_.clear();
}

void SceneSuggestionReview::leaveEditor() {
  panelVisible_ = false;
  selectedId_.reset();
}

bool SceneSuggestionReview::togglePanel(
    const std::vector<Suggestion>& suggestions, const Timeline& timeline) {
  panelVisible_ = !panelVisible_;
  if (panelVisible_) {
    ensureSelection(suggestions, timeline);
  } else {
    selectedId_.reset();
  }
  return panelVisible_;
}

void SceneSuggestionReview::closePanel() {
  panelVisible_ = false;
  selectedId_.reset();
}

SceneSuggestionFilter SceneSuggestionReview::cycleFilter(
    const std::vector<Suggestion>& suggestions, const Timeline& timeline) {
  filter_ = nextSceneSuggestionFilter(filter_);
  ensureSelection(suggestions, timeline);
  return filter_;
}

size_t SceneSuggestionReview::filteredCount(
    const std::vector<Suggestion>& suggestions,
    const Timeline& timeline) const {
  return visibleSuggestions(suggestions, timeline, filter_).size();
}

const SceneSuggestionReview::Suggestion*
SceneSuggestionReview::selectedSuggestion(
    const std::vector<Suggestion>& suggestions,
    const Timeline& timeline) const {
  if (!panelVisible_ || !selectedId_) return nullptr;
  const auto visible = visibleSuggestions(suggestions, timeline, filter_);
  const auto selected = std::find_if(
      visible.begin(), visible.end(),
      [&](const Suggestion* suggestion) {
        return suggestion->id == *selectedId_;
      });
  return selected == visible.end() ? nullptr : *selected;
}

bool SceneSuggestionReview::select(
    uint64_t id, const std::vector<Suggestion>& suggestions,
    const Timeline& timeline) {
  if (!panelVisible_) return false;
  const auto visible = visibleSuggestions(suggestions, timeline, filter_);
  const auto selected = std::find_if(
      visible.begin(), visible.end(),
      [id](const Suggestion* suggestion) { return suggestion->id == id; });
  if (selected == visible.end()) return false;
  selectedId_ = id;
  return true;
}

void SceneSuggestionReview::clearSelection() { selectedId_.reset(); }

const SceneSuggestionReview::Suggestion* SceneSuggestionReview::navigate(
    const std::vector<Suggestion>& suggestions, const Timeline& timeline,
    int64_t sourcePositionUs, SceneSuggestionNavigation direction) {
  if (!panelVisible_) return nullptr;
  const auto visible = visibleSuggestions(suggestions, timeline, filter_);
  if (visible.empty()) {
    selectedId_.reset();
    return nullptr;
  }

  size_t target = 0;
  bool selectedRelativeTarget = false;
  if (selectedId_) {
    const auto current = std::find_if(
        visible.begin(), visible.end(), [&](const Suggestion* suggestion) {
          return suggestion->id == *selectedId_;
        });
    if (current != visible.end()) {
      const size_t currentIndex =
          static_cast<size_t>(std::distance(visible.begin(), current));
      target = direction == SceneSuggestionNavigation::Previous
                   ? (currentIndex == 0 ? visible.size() - 1
                                        : currentIndex - 1)
                   : (currentIndex + 1) % visible.size();
      selectedRelativeTarget = true;
    }
  }
  if (!selectedRelativeTarget &&
      direction == SceneSuggestionNavigation::Previous) {
    target = visible.size() - 1;
    for (size_t index = visible.size(); index > 0; --index) {
      if (visible[index - 1]->startUs < sourcePositionUs) {
        target = index - 1;
        break;
      }
    }
  } else if (!selectedRelativeTarget) {
    for (size_t index = 0; index < visible.size(); ++index) {
      if (visible[index]->startUs > sourcePositionUs) {
        target = index;
        break;
      }
    }
  }
  selectedId_ = visible[target]->id;
  return visible[target];
}

void SceneSuggestionReview::showCurrentOrFirst(
    const std::vector<Suggestion>& suggestions, const Timeline& timeline,
    int64_t sourcePositionUs) {
  panelVisible_ = true;
  const auto visible = visibleSuggestions(suggestions, timeline, filter_);
  if (visible.empty()) {
    selectedId_.reset();
    return;
  }
  const auto current = std::find_if(
      visible.begin(), visible.end(), [&](const Suggestion* suggestion) {
        return suggestion->startUs <= sourcePositionUs &&
               suggestion->endUs > sourcePositionUs;
      });
  selectedId_ = (current == visible.end() ? visible.front() : *current)->id;
}

bool SceneSuggestionReview::hideSelected(
    const std::vector<Suggestion>& suggestions, const Timeline& timeline) {
  const auto visible = visibleSuggestions(suggestions, timeline, filter_);
  if (!selectedId_) return false;
  const auto selected = std::find_if(
      visible.begin(), visible.end(),
      [&](const Suggestion* suggestion) {
        return suggestion->id == *selectedId_;
      });
  if (selected == visible.end()) {
    selectedId_.reset();
    return false;
  }

  const size_t selectedIndex =
      static_cast<size_t>(std::distance(visible.begin(), selected));
  const uint64_t hiddenId = (*selected)->id;
  if (hiddenIds_.insert(hiddenId).second) {
    hiddenHistory_.push_back(hiddenId);
  }
  const auto remaining = visibleSuggestions(suggestions, timeline, filter_);
  selectedId_ =
      remaining.empty()
          ? std::optional<uint64_t>{}
          : std::optional<uint64_t>{
                remaining[std::min(selectedIndex, remaining.size() - 1)]->id};
  return true;
}

std::optional<uint64_t> SceneSuggestionReview::undoHide(
    const std::vector<Suggestion>& suggestions, const Timeline& timeline) {
  while (!hiddenHistory_.empty()) {
    const uint64_t id = hiddenHistory_.back();
    hiddenHistory_.pop_back();
    if (hiddenIds_.erase(id) == 0) continue;
    const auto restored = std::find_if(
        suggestions.begin(), suggestions.end(),
        [id](const Suggestion& suggestion) { return suggestion.id == id; });
    if (restored == suggestions.end() ||
        !sceneSuggestionVisibleOnTimeline(*restored, timeline)) {
      continue;
    }
    const SceneSuggestionKind kind =
        projectSceneSuggestionKind(restored->kind);
    if (!sceneSuggestionMatchesFilter(kind, filter_)) {
      filter_ = SceneSuggestionFilter::All;
    }
    panelVisible_ = true;
    selectedId_ = id;
    return id;
  }
  return std::nullopt;
}

void SceneSuggestionReview::reconcile(
    const std::vector<Suggestion>& suggestions, const Timeline& timeline) {
  ensureSelection(suggestions, timeline);
}

SceneSuggestionReviewSnapshot SceneSuggestionReview::snapshot(
    const std::vector<Suggestion>& suggestions, const Timeline& timeline,
    bool editorActive) const {
  SceneSuggestionReviewSnapshot out;
  out.visible = editorActive && panelVisible_;
  out.filter = filter_;
  out.canUndoHide = std::any_of(
      hiddenHistory_.begin(), hiddenHistory_.end(), [&](uint64_t id) {
        if (hiddenIds_.count(id) == 0) return false;
        const auto hidden = std::find_if(
            suggestions.begin(), suggestions.end(),
            [id](const Suggestion& suggestion) { return suggestion.id == id; });
        return hidden != suggestions.end() &&
               sceneSuggestionVisibleOnTimeline(*hidden, timeline);
      });
  const auto all =
      visibleSuggestions(suggestions, timeline, SceneSuggestionFilter::All);
  const auto filtered = visibleSuggestions(suggestions, timeline, filter_);
  out.totalCount = all.size();
  out.filteredCount = filtered.size();
  if (!out.visible) return out;

  out.suggestions.reserve(filtered.size());
  for (const Suggestion* suggestion : filtered) {
    SceneSuggestionSnapshot projected = projectSceneSuggestion(
        *suggestion, timeline, selectedId_ == suggestion->id);
    if (!projected.spans.empty()) {
      out.suggestions.push_back(std::move(projected));
    }
  }
  const auto selected = std::find_if(
      filtered.begin(), filtered.end(), [&](const Suggestion* suggestion) {
        return selectedId_ && suggestion->id == *selectedId_;
      });
  if (selected != filtered.end()) {
    out.selectedId = (*selected)->id;
    out.selectedOrdinal =
        static_cast<size_t>(std::distance(filtered.begin(), selected)) + 1;
  }
  return out;
}

std::vector<const SceneSuggestionReview::Suggestion*>
SceneSuggestionReview::visibleSuggestions(
    const std::vector<Suggestion>& suggestions, const Timeline& timeline,
    SceneSuggestionFilter filter) const {
  std::vector<const Suggestion*> visible;
  visible.reserve(suggestions.size());
  for (const Suggestion& suggestion : suggestions) {
    if (hiddenIds_.count(suggestion.id) == 0 &&
        sceneSuggestionMatchesFilter(
            projectSceneSuggestionKind(suggestion.kind), filter) &&
        sceneSuggestionVisibleOnTimeline(suggestion, timeline)) {
      visible.push_back(&suggestion);
    }
  }
  return visible;
}

void SceneSuggestionReview::ensureSelection(
    const std::vector<Suggestion>& suggestions, const Timeline& timeline) {
  if (!panelVisible_) {
    selectedId_.reset();
    return;
  }
  const auto visible = visibleSuggestions(suggestions, timeline, filter_);
  if (visible.empty()) {
    selectedId_.reset();
    return;
  }
  if (selectedId_ &&
      std::any_of(visible.begin(), visible.end(),
                  [&](const Suggestion* suggestion) {
                    return suggestion->id == *selectedId_;
                  })) {
    return;
  }
  selectedId_ = visible.front()->id;
}

}  // namespace playback_video_edit
