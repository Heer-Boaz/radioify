#pragma once

#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

#include "playback/video/analysis/scene_analysis.h"
#include "playback/video/edit/timeline.h"
#include "playback/video/edit/view.h"

namespace playback_video_edit {

enum class SceneSuggestionNavigation : uint8_t {
  Previous,
  Next,
};

// Owns the complete interaction state for reviewing immutable analysis
// suggestions. The editor workspace supplies analysis data and a timeline;
// renderers consume only SceneSuggestionReviewSnapshot.
class SceneSuggestionReview {
 public:
  using Suggestion = playback_video_analysis::SceneSuggestion;

  bool panelVisible() const { return panelVisible_; }
  SceneSuggestionFilter filter() const { return filter_; }
  std::optional<uint64_t> selectedId() const { return selectedId_; }

  void beginAnalysis();
  void leaveEditor();
  bool togglePanel(const std::vector<Suggestion>& suggestions,
                   const Timeline& timeline);
  void closePanel();
  SceneSuggestionFilter cycleFilter(
      const std::vector<Suggestion>& suggestions, const Timeline& timeline);
  size_t filteredCount(const std::vector<Suggestion>& suggestions,
                       const Timeline& timeline) const;

  const Suggestion* selectedSuggestion(
      const std::vector<Suggestion>& suggestions,
      const Timeline& timeline) const;
  bool select(uint64_t id, const std::vector<Suggestion>& suggestions,
              const Timeline& timeline);
  void clearSelection();
  const Suggestion* navigate(const std::vector<Suggestion>& suggestions,
                             const Timeline& timeline,
                             int64_t sourcePositionUs,
                             SceneSuggestionNavigation direction);
  void showCurrentOrFirst(const std::vector<Suggestion>& suggestions,
                          const Timeline& timeline,
                          int64_t sourcePositionUs);
  bool hideSelected(const std::vector<Suggestion>& suggestions,
                    const Timeline& timeline);
  std::optional<uint64_t> undoHide(
      const std::vector<Suggestion>& suggestions, const Timeline& timeline);
  void reconcile(const std::vector<Suggestion>& suggestions,
                 const Timeline& timeline);

  SceneSuggestionReviewSnapshot snapshot(
      const std::vector<Suggestion>& suggestions, const Timeline& timeline,
      bool editorActive) const;

 private:
  std::vector<const Suggestion*> visibleSuggestions(
      const std::vector<Suggestion>& suggestions, const Timeline& timeline,
      SceneSuggestionFilter filter) const;
  void ensureSelection(const std::vector<Suggestion>& suggestions,
                       const Timeline& timeline);

  bool panelVisible_ = false;
  SceneSuggestionFilter filter_ = SceneSuggestionFilter::All;
  std::optional<uint64_t> selectedId_;
  std::unordered_set<uint64_t> hiddenIds_;
  std::vector<uint64_t> hiddenHistory_;
};

}  // namespace playback_video_edit
