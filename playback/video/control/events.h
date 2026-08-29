#pragma once

#include <cstdint>
#include <memory>

#include "playback/video/frame_step.h"
#include "playback/video/frame_step_seek_plan.h"
#include "playback/video/sequence.h"

namespace playback_video_control {

enum class EventType {
  SeekRequest,
  FrameStepRequest,
  FrameStepSeekRequest,
  PauseRequest,
  ResizeRequest,
  SetSequence,
  UpdateComposition,
  CycleAudioTrack,
  SeekApplied,
  FirstFramePresented,
};

struct Event {
  EventType type = EventType::SeekRequest;
  int64_t arg1 = 0;
  int64_t arg2 = 0;
  int serial = 0;
  uint64_t seekRequestGeneration = 0;
  uint64_t frameStepGeneration = 0;
  // Program-timeline target committed with the immutable sequence projection.
  // The control thread must not infer it later from a possibly newer frame.
  int64_t sequencePositionUs = 0;
  playback_video_frame_step_seek::Plan frameStepSeek;
  std::shared_ptr<const playback_video_sequence::Timeline> sequence;
  playback_video_frame_step::Direction frameStepDirection =
      playback_video_frame_step::Direction::Next;
};

inline bool shouldCoalesceQueuedEvent(EventType queuedTail,
                                      EventType incoming) {
  return (queuedTail == EventType::SeekRequest &&
          incoming == EventType::SeekRequest) ||
         (queuedTail == EventType::SetSequence &&
          incoming == EventType::SetSequence) ||
         (queuedTail == EventType::UpdateComposition &&
          incoming == EventType::UpdateComposition);
}

}  // namespace playback_video_control
