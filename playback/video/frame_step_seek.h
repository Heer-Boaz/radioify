#pragma once

#include <mutex>
#include <optional>

#include "playback/video/frame_step_seek_plan.h"

namespace playback_video_frame_step_seek {

class Controller {
 public:
  void reset();
  void publishForSerial(int serial, const Plan& plan);
  std::optional<Plan> consumeForSerial(int serial);

 private:
  std::mutex mutex_;
  int serial_ = 0;
  Plan plan_;
};

}  // namespace playback_video_frame_step_seek
