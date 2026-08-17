#pragma once

#include <cstddef>
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
  const std::vector<CutTransition>& cutTransitions() const {
    return cutTransitions_;
  }
  DecisionList decisionList() const { return {keptRanges_, cutTransitions_}; }
  bool isUnmodified() const;

  // Intersect the current sequence with keep. Returns false when the request
  // is invalid, is a no-op, or would create an empty sequence.
  bool canTrimTo(SourceRange keep) const;
  bool trimTo(SourceRange keep);

  // Subtract remove from every kept range and close the resulting timeline
  // gap (ripple delete). Returns false for an empty/no-op selection or when
  // the complete sequence would be removed.
  bool canRippleDelete(SourceRange remove) const;
  bool rippleDelete(SourceRange remove);

  std::optional<size_t> nearestCutIndex(int64_t timelineUs,
                                        int64_t toleranceUs) const;
  bool setCutTransition(size_t cutIndex, CutTransition transition);

  bool containsSourceTime(int64_t sourceUs) const;
  std::optional<int64_t> nextKeptSourceTime(int64_t sourceUs) const;
  std::optional<int64_t> previousKeptSourceTime(int64_t sourceUs) const;

 private:
  bool replaceRanges(std::vector<SourceRange> ranges);

  int64_t sourceDurationUs_ = 0;
  std::vector<SourceRange> keptRanges_;
  std::vector<CutTransition> cutTransitions_;
};

// Persistent edit decisions and their history. UI activation, selection, and
// playhead state are deliberately not part of the document.
class Document {
 public:
  void load(int64_t sourceDurationUs);
  bool trimTo(SourceRange keep);
  bool rippleDelete(SourceRange remove);
  bool setCutTransition(size_t cutIndex, CutTransition transition);
  bool undo();
  bool redo();
  bool resetEdits();
  bool discardAllChanges();
  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  bool hasUnexportedChanges() const;

  // A completed export may represent an older revision when editing continued
  // in parallel. Keep that exact decision list as the clean baseline.
  void markExported(const DecisionList& decisions);

  const Timeline& timeline() const { return timeline_; }

 private:
  bool commit(Timeline next);

  Timeline timeline_;
  std::vector<Timeline> undo_;
  std::vector<Timeline> redo_;
  // Successful outputs are immutable artifacts. Exporting a newer decision
  // list must not make an older, already exported revision dirty again.
  std::vector<DecisionList> exportedRevisions_;
};

// Ephemeral timeline-controller state. Marks never affect document dirty
// state and are cleared when a document operation consumes them.
class Selection {
 public:
  bool hasMarks() const { return inUs_.has_value() || outUs_.has_value(); }
  bool clear();
  bool clear(EditBoundary boundary);
  void markIn(const Timeline& timeline, int64_t sourceUs,
              int64_t minimumSelectionDurationUs = 1);
  void markOut(const Timeline& timeline, int64_t sourceFrameStartUs,
               int64_t sourceFrameEndUs,
               int64_t minimumSelectionDurationUs = 1);
  bool moveBoundary(const Timeline& timeline, EditBoundary boundary,
                    int64_t timelineUs,
                    int64_t minimumSelectionDurationUs);

  // Both edit operations consume one explicit two-sided range. This keeps the
  // range-selection tool independent from the operation chosen afterwards.
  std::optional<SourceRange> range() const;
  std::optional<int64_t> inSourceUs() const { return inUs_; }
  std::optional<int64_t> outSourceUs() const { return outUs_; }
  std::optional<int64_t> outFrameSourceUs() const { return outFrameUs_; }

 private:
  void setOutBoundary(const Timeline& timeline, int64_t sourceUsExclusive,
                      int64_t sourceFrameStartUs);

  std::optional<int64_t> inUs_;
  std::optional<int64_t> outUs_;
  std::optional<int64_t> outFrameUs_;
};

// Pure projection for renderers and menus. No caller maintains a shadow copy.
EditSnapshot buildSnapshot(const Document& document,
                           const Selection& selection, bool active,
                           std::optional<int64_t> playheadTimelineUs = {},
                           int64_t timecodeFrameDurationUs = 0);

}  // namespace playback_video_edit
