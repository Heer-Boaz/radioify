#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "playback/video/edit/view.h"

namespace playback_video_edit {

class Timeline {
 public:
  Timeline() = default;
  explicit Timeline(int64_t sourceDurationUs);

  void reset(int64_t sourceDurationUs);

  int64_t sourceDurationUs() const { return sourceDurationUs_; }
  int64_t outputDurationUs() const;
  const std::vector<SourceRange>& keptRanges() const { return keptRanges_; }
  bool isUnmodified() const;

  // Intersect the current sequence with keep. Returns false when the request
  // is invalid or would create an empty sequence.
  bool trimTo(SourceRange keep);

  // Subtract remove from every kept range and close the resulting timeline
  // gap (ripple delete). Returns false for an empty/no-op selection or when
  // the complete sequence would be removed.
  bool rippleDelete(SourceRange remove);

  bool containsSourceTime(int64_t sourceUs) const;
  std::optional<int64_t> nextKeptSourceTime(int64_t sourceUs) const;
  std::optional<int64_t> previousKeptSourceTime(int64_t sourceUs) const;

 private:
  bool replaceRanges(std::vector<SourceRange> ranges);

  int64_t sourceDurationUs_ = 0;
  std::vector<SourceRange> keptRanges_;
};

// Session-level editing state. Undo/redo stores exact timeline values instead
// of reconstructing cuts heuristically.
class EditSession {
 public:
  void activate(int64_t sourceDurationUs);
  void deactivate();
  bool active() const { return active_; }

  void markIn(int64_t sourceUs);
  void markOut(int64_t sourceUsExclusive);
  bool moveBoundary(EditBoundary boundary, int64_t timelineUs,
                    int64_t minimumSelectionDurationUs);

  bool trimToSelection();
  bool rippleDeleteSelection();
  bool undo();
  bool redo();
  bool resetEdits();
  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }

  const Timeline& timeline() const { return timeline_; }
  EditSnapshot snapshot() const;

 private:
  std::optional<SourceRange> selection() const;
  bool commit(Timeline next);
  int64_t clampSourceTime(int64_t sourceUs) const;

  bool active_ = false;
  Timeline timeline_;
  std::optional<int64_t> inUs_;
  std::optional<int64_t> outUs_;
  std::vector<Timeline> undo_;
  std::vector<Timeline> redo_;
};

}  // namespace playback_video_edit
