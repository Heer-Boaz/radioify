#include "playback/video/edit/scene_suggestion_review.h"
#include "playback/video/edit/suggestion_preview.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "scene_suggestion_review_tests: " << message << '\n';
  return false;
}

playback_video_analysis::EditProposal suggestion(
    uint64_t id, int64_t startUs, int64_t endUs,
    playback_video_analysis::EditDisposition kind) {
  playback_video_analysis::EditProposal out;
  out.id = id;
  out.startUs = startUs;
  out.endUs = endUs;
  out.disposition = kind;
  out.reason = "Observed activity";
  return out;
}

}  // namespace

int main() {
  namespace analysis = playback_video_analysis;
  namespace edit = playback_video_edit;
  bool ok = true;

  constexpr int64_t second = 1'000'000;
  const std::vector<analysis::EditProposal> suggestions = {
      suggestion(1, 0 * second, 20 * second, analysis::EditDisposition::Review),
      suggestion(2, 20 * second, 40 * second, analysis::EditDisposition::Keep),
      suggestion(3, 40 * second, 60 * second, analysis::EditDisposition::Shorten),
      suggestion(4, 60 * second, 80 * second, analysis::EditDisposition::Keep),
      suggestion(5, 80 * second, 100 * second,
                 analysis::EditDisposition::Review),
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
                       edit::SceneSuggestionFilter::Keep &&
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
                       edit::SceneSuggestionFilter::Shorten &&
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
                   review.selectedId() == std::optional<uint64_t>(4),
               "reopening review must retain its selected suggestion");
  review.select(1, suggestions, timeline);

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

  edit::SuggestionPreview preview;
  using Update = edit::SuggestionPreview::Update;
  ok &= expect(!preview.start(-1, second, 1) && !preview.start(second, second, 1) &&
                   !preview.active(), "invalid preview bounds must not activate transport policy");
  ok &= expect(preview.start(2 * second, 4 * second, 7) && preview.active(),
               "a preview must own its program-time end and its seek generation");
  ok &= expect(preview.observe(9 * second, 7, 6, true, false) == Update::None &&
                   preview.observe(9 * second, 7, 6, false, false) == Update::None && preview.active(),
               "the old playhead must not end a preview before its seek is acknowledged");
  ok &= expect(preview.observe(3 * second, 7, 7, false, false) == Update::None &&
                   preview.observe(4 * second, 7, 7, false, false) == Update::EndReached &&
                   !preview.active() && preview.observe(5 * second, 7, 7, false, false) == Update::None,
               "preview completion must request one synchronized pause at its end");
  preview.start(2 * second, 4 * second, 8);
  ok &= expect(preview.observe(6 * second, 9, 8, true, false) == Update::Superseded &&
                   !preview.active(), "a user seek must disarm preview without pausing that new seek");
  preview.start(2 * second, 4 * second, 10);
  ok &= expect(preview.stop() && !preview.stop() &&
                   preview.observe(5 * second, 10, 10, false, false) == Update::None,
               "stopping a preview must remove its end policy exactly once");
  preview.start(2 * second, 4 * second, 11);
  ok &= expect(preview.observe(4 * second - 1, 11, 11, false, true) == Update::EndReached &&
                   !preview.active(), "EOF must complete the final preview even when the last frame precedes duration");

  return ok ? 0 : 1;
}
