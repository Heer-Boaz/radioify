#pragma once

#include <cstdint>

#include "playback/video/frame_step.h"
#include "playback/video/frame_step_seek_plan.h"

namespace playback_video_control {

enum class EventType {
  SeekRequest,
  FrameStepRequest,
  FrameStepSeekRequest,
  PauseRequest,
  ResizeRequest,
  CycleAudioTrack,
  CloseRequest,
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
  playback_video_frame_step_seek::Plan frameStepSeek;
  playback_video_frame_step::Direction frameStepDirection =
      playback_video_frame_step::Direction::Next;
};

inline bool shouldCoalesceQueuedEvent(EventType queuedTail,
                                      EventType incoming) {
  return queuedTail == EventType::SeekRequest &&
         incoming == EventType::SeekRequest;
}

}  // namespace playback_video_control
