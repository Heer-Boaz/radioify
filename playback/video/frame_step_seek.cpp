#include "frame_step_seek.h"

#include <cassert>

namespace playback_video_frame_step_seek {

void Controller::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  serial_ = 0;
  plan_ = {};
}

void Controller::publishForSerial(int serial, const Plan& plan) {
  assert(serial > 0);
  assert(plan.valid());
  std::lock_guard<std::mutex> lock(mutex_);
  serial_ = serial;
  plan_ = plan;
}

std::optional<Plan> Controller::consumeForSerial(int serial) {
  assert(serial > 0);
  std::lock_guard<std::mutex> lock(mutex_);
  if (serial_ == serial) {
    Plan plan = plan_;
    serial_ = 0;
    plan_ = {};
    return plan;
  }
  if (serial_ != 0 && serial_ < serial) {
    serial_ = 0;
    plan_ = {};
  }
  return std::nullopt;
}

}  // namespace playback_video_frame_step_seek
