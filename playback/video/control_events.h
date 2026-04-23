#pragma once

#include <cstdint>

namespace playback_control {

enum class EventType {
  SeekRequest,
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
};

}  // namespace playback_control
