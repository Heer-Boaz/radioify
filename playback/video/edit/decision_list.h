#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "playback/video/sequence.h"

namespace playback_video_edit {

using playback_video_sequence::SourceRange;

inline constexpr uint8_t kMinimumSmoothCutFrames = 2;
inline constexpr uint8_t kDefaultSmoothCutFrames = 4;
inline constexpr uint8_t kMaximumSmoothCutFrames = 6;

enum class CutTransitionKind : uint8_t {
  Hard,
  Smooth,
};

struct CutTransition {
  CutTransitionKind kind = CutTransitionKind::Hard;
  uint8_t durationFrames = 0;

  static constexpr CutTransition hard() { return {}; }
  static constexpr CutTransition smooth(
      uint8_t frames = kDefaultSmoothCutFrames) {
    return {CutTransitionKind::Smooth,
            std::clamp(frames, kMinimumSmoothCutFrames,
                       kMaximumSmoothCutFrames)};
  }
};

inline bool operator==(const CutTransition& left,
                       const CutTransition& right) {
  return left.kind == right.kind &&
         left.durationFrames == right.durationFrames;
}

inline bool operator!=(const CutTransition& left,
                       const CutTransition& right) {
  return !(left == right);
}

struct DecisionList {
  std::vector<SourceRange> keptRanges;
  // Entry i owns the edit point between keptRanges[i] and
  // keptRanges[i + 1]. A hard cut is explicit so changing one edit point
  // never becomes a sequence-wide render option.
  std::vector<CutTransition> cutTransitions;

  bool hasValidShape() const {
    const size_t expected = keptRanges.empty() ? 0 : keptRanges.size() - 1;
    if (cutTransitions.size() != expected) return false;
    return std::all_of(cutTransitions.begin(), cutTransitions.end(),
                       [](const CutTransition& transition) {
                         switch (transition.kind) {
                           case CutTransitionKind::Hard:
                             return transition.durationFrames == 0;
                           case CutTransitionKind::Smooth:
                             return transition.durationFrames >=
                                        kMinimumSmoothCutFrames &&
                                    transition.durationFrames <=
                                        kMaximumSmoothCutFrames;
                         }
                         return false;
                       });
  }
};

inline bool operator==(const DecisionList& left, const DecisionList& right) {
  return left.keptRanges == right.keptRanges &&
         left.cutTransitions == right.cutTransitions;
}

inline bool operator!=(const DecisionList& left, const DecisionList& right) {
  return !(left == right);
}

}  // namespace playback_video_edit
