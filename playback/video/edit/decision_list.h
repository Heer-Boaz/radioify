#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "playback/video/sequence.h"

namespace playback_video_edit {

using playback_video_sequence::SourceRange;

inline constexpr uint8_t kMinimumSmoothCutFrames =
    playback_video_sequence::kMinimumTransitionFrames;
inline constexpr uint8_t kDefaultSmoothCutFrames =
    playback_video_sequence::kDefaultTransitionFrames;
inline constexpr uint8_t kMaximumSmoothCutFrames =
    playback_video_sequence::kMaximumTransitionFrames;

using CutTransitionKind = playback_video_sequence::TransitionKind;
using CutTransition = playback_video_sequence::Transition;

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
                             return transition.outgoingFrames == 0 &&
                                    transition.incomingFrames == 0;
                           case CutTransitionKind::MotionSmooth:
                             return transition.outgoingFrames > 0 &&
                                    transition.incomingFrames > 0 &&
                                    transition.durationFrames() >=
                                        kMinimumSmoothCutFrames &&
                                    transition.durationFrames() <=
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
