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

struct FrameMapping {
  int64_t presentationPtsUs = 0;
  int64_t presentationDurationUs = 0;
};

struct AudioSlice {
  uint64_t sourceOffsetFrames = 0;
  uint64_t frameCount = 0;
  int64_t presentationPtsUs = 0;
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
  // Resolve a decode start inside actual content. The timeline end remains a
  // useful boundary for editing, but it is not itself a presentable frame.
  Point pointAtPlaybackPosition(int64_t presentationUs) const;
  std::optional<size_t> clipIndexAtSource(int64_t sourceUs) const;
  std::optional<Point> pointForSource(int64_t sourceUs,
                                      SourceBias bias) const;
  std::optional<FrameMapping> mapFrame(int64_t sourcePtsUs,
                                       int64_t sourceDurationUs) const;
  std::optional<AudioSlice> sliceAudio(int64_t sourcePtsUs,
                                       uint64_t frameCount,
                                       uint32_t sampleRate) const;

 private:
  int64_t sourceDurationUs_ = 0;
  int64_t durationUs_ = 0;
  std::vector<Clip> clips_;
};

}  // namespace playback_video_sequence
