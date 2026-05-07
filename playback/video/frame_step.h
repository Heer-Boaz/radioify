#pragma once

#include <cstdint>

namespace playback_video_frame_step {

enum class Direction {
  Previous,
  Next,
};

struct Request {
  Direction direction = Direction::Next;
  int serial = 0;
  uint64_t generation = 0;
};

}  // namespace playback_video_frame_step
