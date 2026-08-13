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

// Persistent edit decisions and their history. UI activation, selection, and
// playhead state are deliberately not part of the document.
class Document {
 public:
  void load(int64_t sourceDurationUs);
  bool trimTo(SourceRange keep);
  bool rippleDelete(SourceRange remove);
  bool undo();
  bool redo();
  bool resetEdits();
  bool discardAllChanges();
  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  bool hasUnexportedChanges() const;

  // A completed export may represent an older revision when editing continued
  // in parallel. Keep that exact decision list as the clean baseline.
  void markExported(const std::vector<SourceRange>& ranges);

  const Timeline& timeline() const { return timeline_; }

 private:
  bool commit(Timeline next);

  Timeline timeline_;
  std::vector<Timeline> undo_;
  std::vector<Timeline> redo_;
  std::optional<std::vector<SourceRange>> exportedRanges_;
};

// Ephemeral timeline-controller state. Marks never affect document dirty
// state and are cleared when a document operation consumes them.
class Selection {
 public:
  void clear();
  void markIn(const Timeline& timeline, int64_t sourceUs);
  void markOut(const Timeline& timeline, int64_t sourceUsExclusive);
  bool moveBoundary(const Timeline& timeline, EditBoundary boundary,
                    int64_t timelineUs,
                    int64_t minimumSelectionDurationUs);

  std::optional<SourceRange> range() const;
  std::optional<int64_t> inSourceUs() const { return inUs_; }
  std::optional<int64_t> outSourceUs() const { return outUs_; }

 private:
  std::optional<int64_t> inUs_;
  std::optional<int64_t> outUs_;
};

// Pure projection for renderers and menus. No caller maintains a shadow copy.
EditSnapshot buildSnapshot(const Document& document,
                           const Selection& selection, bool active,
                           std::optional<int64_t> playheadTimelineUs = {},
                           int64_t frameDurationUs = 0);

}  // namespace playback_video_edit
