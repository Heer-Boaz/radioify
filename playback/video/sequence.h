#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace playback_video_sequence {

// Half-open interval in the immutable source media.
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

struct Clip {
  SourceRange source;
  int64_t presentationStartUs = 0;

  int64_t presentationEndUs() const {
    return presentationStartUs + source.durationUs();
  }
};

struct Point {
  size_t clipIndex = 0;
  int64_t sourceUs = 0;
  int64_t presentationUs = 0;
};

enum class SourceBias {
  Forward,
  Backward,
};

// Immutable edit-decision-list projection. Playback time is the concatenation
// of the source clips; source time remains available for exact decode seeks and
// edit marks.
class Timeline {
 public:
  static std::optional<Timeline> create(
      int64_t sourceDurationUs, const std::vector<SourceRange>& ranges);

  int64_t sourceDurationUs() const { return sourceDurationUs_; }
  int64_t durationUs() const { return durationUs_; }
  bool isIdentity() const;
  const std::vector<Clip>& clips() const { return clips_; }

  Point pointAt(int64_t presentationUs) const;
  std::optional<Point> pointForSource(int64_t sourceUs,
                                      SourceBias bias) const;

 private:
  int64_t sourceDurationUs_ = 0;
  int64_t durationUs_ = 0;
  std::vector<Clip> clips_;
};

}  // namespace playback_video_sequence
