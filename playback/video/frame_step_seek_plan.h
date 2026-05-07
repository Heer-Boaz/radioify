#pragma once

#include <cstdint>

#include "playback/video/frame_step.h"

namespace playback_video_frame_step_seek {

inline constexpr int64_t kFrameStepDecodeLeadInUs = 1000000;

inline int64_t frameStepDemuxLeadInUs(int64_t anchorUs) {
  return anchorUs > kFrameStepDecodeLeadInUs
             ? anchorUs - kFrameStepDecodeLeadInUs
             : 0;
}

enum class PlanMode {
  ExactFrame,
  PreviousBeforeTarget,
};

struct FrameRecord {
  int64_t ptsUs = 0;
  int64_t durationUs = 0;
  uint64_t serial = 0;
  uint64_t displayIndex = 0;
  uint64_t logicalIndex = 0;

  bool valid() const {
    return logicalIndex > 0 && durationUs > 0 && ptsUs >= 0;
  }
};

struct Plan {
  PlanMode mode = PlanMode::ExactFrame;
  playback_video_frame_step::Direction direction =
      playback_video_frame_step::Direction::Next;
  uint64_t generation = 0;
  FrameRecord target;
  FrameRecord anchor;

  bool valid() const {
    return target.valid() && anchor.valid();
  }

  int64_t seekTargetUs() const {
    return target.ptsUs;
  }

  int64_t demuxTargetUs() const {
    if (mode == PlanMode::PreviousBeforeTarget) {
      return frameStepDemuxLeadInUs(target.ptsUs);
    }
    return frameStepDemuxLeadInUs(anchor.ptsUs);
  }

  int64_t demuxWindowEndUs() const {
    return target.ptsUs;
  }

  int64_t decoderPrerollTargetUs() const {
    if (mode == PlanMode::PreviousBeforeTarget) {
      return 0;
    }
    return anchor.ptsUs;
  }
};

}  // namespace playback_video_frame_step_seek
