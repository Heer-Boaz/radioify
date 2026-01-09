#include "clock.h"

void Clock::set(int64_t new_pts_us, int64_t now_us) {
  std::lock_guard<std::mutex> lock(mutex_);
  pts_us_ = new_pts_us;
  last_updated_us_ = now_us;
  if (paused_) {
    pause_pts_us_ = new_pts_us;
  }
}

int64_t Clock::get(int64_t now_us) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (paused_) {
    return pause_pts_us_;
  }
  int64_t delta = now_us - last_updated_us_;
  return pts_us_ + static_cast<int64_t>(delta * speed_);
}

void Clock::set_paused(bool paused, int64_t now_us) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (paused_ == paused) return;
  if (paused) {
    pause_pts_us_ = pts_us_ + (now_us - last_updated_us_);
  } else {
    pts_us_ = pause_pts_us_;
    last_updated_us_ = now_us;
  }
  paused_ = paused;
}

void Clock::set_speed(double new_speed, int64_t now_us) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (new_speed <= 0.0 || new_speed == speed_) return;
  if (!paused_) {
    pts_us_ = pts_us_ + static_cast<int64_t>((now_us - last_updated_us_) * speed_);
    last_updated_us_ = now_us;
  }
  speed_ = new_speed;
}
