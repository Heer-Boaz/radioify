#include "playback/video/edit/scene_suggestion_review.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "scene_suggestion_review_tests: " << message << '\n';
  return false;
}

playback_video_analysis::SceneSuggestion suggestion(
    uint64_t id, int64_t startUs, int64_t endUs,
    playback_video_analysis::SceneKind kind) {
  playback_video_analysis::SceneSuggestion out;
  out.id = id;
  out.startUs = startUs;
  out.endUs = endUs;
  out.kind = kind;
  out.confidence = 0.8f;
  return out;
}

}  // namespace

int main() {
  namespace analysis = playback_video_analysis;
  namespace edit = playback_video_edit;
  bool ok = true;

  constexpr int64_t second = 1'000'000;
  const std::vector<analysis::SceneSuggestion> suggestions = {
      suggestion(1, 0 * second, 20 * second, analysis::SceneKind::Gameplay),
      suggestion(2, 20 * second, 40 * second, analysis::SceneKind::Cutscene),
      suggestion(3, 40 * second, 60 * second, analysis::SceneKind::Dialogue),
      suggestion(4, 60 * second, 80 * second, analysis::SceneKind::Cutscene),
      suggestion(5, 80 * second, 100 * second,
                 analysis::SceneKind::MenuOrLoading),
  };
  edit::Timeline timeline(100 * second);
  edit::SceneSuggestionReview review;

  review.beginAnalysis();
  review.showCurrentOrFirst(suggestions, timeline, 65 * second);
  auto snapshot = review.snapshot(suggestions, timeline, true);
  ok &= expect(snapshot.visible && snapshot.totalCount == 5 &&
                   snapshot.filteredCount == 5 &&
                   snapshot.selectedId == std::optional<uint64_t>(4) &&
                   snapshot.selectedOrdinal == std::optional<size_t>(4) &&
                   snapshot.suggestions.size() == 5,
               "analysis completion must open one coherent review snapshot "
               "around the current playhead");

  ok &= expect(review.cycleFilter(suggestions, timeline) ==
                       edit::SceneSuggestionFilter::Cutscenes &&
                   review.filteredCount(suggestions, timeline) == 2 &&
                   review.selectedId() == std::optional<uint64_t>(4),
               "filtering must retain a selection that remains visible");
  const auto* next = review.navigate(
      suggestions, timeline, 65 * second,
      edit::SceneSuggestionNavigation::Next);
  const auto* previous = review.navigate(
      suggestions, timeline, 65 * second,
      edit::SceneSuggestionNavigation::Previous);
  ok &= expect(next && next->id == 2 && previous && previous->id == 4,
               "filtered navigation must wrap deterministically");

  ok &= expect(review.hideSelected(suggestions, timeline),
               "the selected suggestion must be hideable");
  snapshot = review.snapshot(suggestions, timeline, true);
  ok &= expect(snapshot.totalCount == 4 && snapshot.filteredCount == 1 &&
                   snapshot.selectedId == std::optional<uint64_t>(2) &&
                   snapshot.canUndoHide,
               "Hide must select the nearest remaining item and publish Undo");

  ok &= expect(review.cycleFilter(suggestions, timeline) ==
                       edit::SceneSuggestionFilter::Dialogue &&
                   review.selectedId() == std::optional<uint64_t>(3),
               "changing filters must repair selection inside that filter");
  ok &= expect(review.undoHide(suggestions, timeline) ==
                       std::optional<uint64_t>(4) &&
                   review.filter() == edit::SceneSuggestionFilter::All &&
                   review.selectedId() == std::optional<uint64_t>(4),
               "Undo Hide must reveal and focus the restored item even after "
               "the filter changed");

  ok &= expect(!review.togglePanel(suggestions, timeline),
               "closing review must report its resulting visibility");
  snapshot = review.snapshot(suggestions, timeline, true);
  ok &= expect(!snapshot.visible && snapshot.suggestions.empty() &&
                   !snapshot.selectedId,
               "a closed review must not leak stale projected selection");
  ok &= expect(review.togglePanel(suggestions, timeline) &&
                   review.selectedId() == std::optional<uint64_t>(1),
               "reopening review must establish a valid selection");

  ok &= expect(timeline.rippleDelete({0, 20 * second}),
               "the timeline fixture must remove the selected suggestion");
  review.reconcile(suggestions, timeline);
  snapshot = review.snapshot(suggestions, timeline, true);
  ok &= expect(snapshot.totalCount == 4 &&
                   snapshot.selectedId == std::optional<uint64_t>(2),
               "timeline edits must reconcile review selection through the "
               "same owner");

  review.leaveEditor();
  snapshot = review.snapshot(suggestions, timeline, true);
  ok &= expect(!snapshot.visible && !snapshot.selectedId,
               "leaving the editor must close review and clear its focus");

  return ok ? 0 : 1;
}
