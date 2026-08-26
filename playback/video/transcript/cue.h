#pragma once

#include <cstdint>
#include <string>

namespace playback_video_transcript {

struct Segment {
  int64_t startUs = 0;
  int64_t endUs = 0;
  std::string text;
};

}  // namespace playback_video_transcript
