#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace playback_video_edit {

// Source-space, half-open media interval. Edit decisions always reference the
// immutable source; output time is derived by concatenating these intervals.
struct SourceRange {
  int64_t startUs = 0;
  int64_t endUs = 0;

  int64_t durationUs() const { return endUs - startUs; }
};

inline bool operator==(const SourceRange& lhs, const SourceRange& rhs) {
  return lhs.startUs == rhs.startUs && lhs.endUs == rhs.endUs;
}

inline bool operator!=(const SourceRange& lhs, const SourceRange& rhs) {
  return !(lhs == rhs);
}

class Timeline {
 public:
  Timeline() = default;
  explicit Timeline(int64_t sourceDurationUs);

  void reset(int64_t sourceDurationUs);

  int64_t sourceDurationUs() const { return sourceDurationUs_; }
  int64_t outputDurationUs() const;
  const std::vector<SourceRange>& keptRanges() const { return keptRanges_; }
  bool empty() const { return keptRanges_.empty(); }
  bool isUnmodified() const;

  // Intersect the current sequence with keep. Returns false when the request
  // is invalid or would create an empty sequence.
  bool trimTo(SourceRange keep);

  // Subtract remove from every kept range and close the resulting timeline
  // gap (ripple delete). Returns false for an empty/no-op selection or when
  // the complete sequence would be removed.
  bool rippleDelete(SourceRange remove);

  bool containsSourceTime(int64_t sourceUs) const;
  std::optional<SourceRange> rangeContaining(int64_t sourceUs) const;
  std::optional<int64_t> nextKeptSourceTime(int64_t sourceUs) const;
  std::optional<int64_t> previousKeptSourceTime(int64_t sourceUs) const;
  std::optional<int64_t> sourceToOutputTime(int64_t sourceUs) const;
  std::optional<int64_t> outputToSourceTime(int64_t outputUs) const;

 private:
  bool replaceRanges(std::vector<SourceRange> ranges);

  int64_t sourceDurationUs_ = 0;
  std::vector<SourceRange> keptRanges_;
};

struct EditSnapshot {
  bool active = false;
  int64_t sourceDurationUs = 0;
  int64_t outputDurationUs = 0;
  std::vector<SourceRange> keptRanges;
  std::optional<int64_t> inUs;
  std::optional<int64_t> outUs;
  bool canApplySelection = false;
  bool canUndo = false;
  bool canRedo = false;
  uint64_t revision = 0;
};

// Session-level editing state. Timeline revisions are value snapshots so
// undo/redo never reconstructs cuts heuristically.
class EditSession {
 public:
  void activate(int64_t sourceDurationUs);
  void deactivate();
  bool active() const { return active_; }

  void markIn(int64_t sourceUs);
  void markOut(int64_t sourceUsExclusive);
  void clearSelection();
  std::optional<SourceRange> selection() const;

  bool trimToSelection();
  bool rippleDeleteSelection();
  bool undo();
  bool redo();
  bool resetEdits();

  const Timeline& timeline() const { return timeline_; }
  EditSnapshot snapshot() const;

 private:
  bool commit(Timeline next);
  int64_t clampSourceTime(int64_t sourceUs) const;

  bool active_ = false;
  Timeline timeline_;
  std::optional<int64_t> inUs_;
  std::optional<int64_t> outUs_;
  std::vector<Timeline> undo_;
  std::vector<Timeline> redo_;
  uint64_t revision_ = 0;
};

}  // namespace playback_video_edit
