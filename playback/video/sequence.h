#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace playback_video_sequence {

inline constexpr uint8_t kMinimumTransitionFrames = 2;
inline constexpr uint8_t kDefaultTransitionFrames = 4;
inline constexpr uint8_t kMaximumTransitionFrames = 6;

enum class TransitionKind : uint8_t {
  Hard,
  MotionSmooth,
};

struct Transition {
  TransitionKind kind = TransitionKind::Hard;
  // Explicit coterminous overlap contributed by the previous and next clip.
  uint8_t outgoingFrames = 0;
  uint8_t incomingFrames = 0;

  static constexpr Transition hard() { return {}; }
  static constexpr Transition motionSmooth(
      uint16_t frames = kDefaultTransitionFrames) {
    const uint16_t duration = std::clamp<uint16_t>(
        frames, kMinimumTransitionFrames, kMaximumTransitionFrames);
    const uint8_t outgoing = static_cast<uint8_t>(duration / 2);
    return {TransitionKind::MotionSmooth, outgoing,
            static_cast<uint8_t>(duration - outgoing)};
  }

  constexpr uint16_t durationFrames() const {
    return static_cast<uint16_t>(outgoingFrames) +
           static_cast<uint16_t>(incomingFrames);
  }
};

inline bool operator==(const Transition& left, const Transition& right) {
  return left.kind == right.kind &&
         left.outgoingFrames == right.outgoingFrames &&
         left.incomingFrames == right.incomingFrames;
}

inline bool operator!=(const Transition& left, const Transition& right) {
  return !(left == right);
}

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
      int64_t sourceDurationUs, const std::vector<SourceRange>& ranges,
      const std::vector<Transition>& transitions = {});

  int64_t sourceDurationUs() const { return sourceDurationUs_; }
  int64_t durationUs() const { return durationUs_; }
  bool isIdentity() const;
  const std::vector<Clip>& clips() const { return clips_; }
  const std::vector<Transition>& transitions() const { return transitions_; }

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
  std::vector<Transition> transitions_;
};

}  // namespace playback_video_sequence
