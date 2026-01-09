#pragma once

#include <cstdint>
#include <mutex>

struct Clock {
  void set(int64_t new_pts_us, int64_t now_us);
  int64_t get(int64_t now_us);
  void set_paused(bool paused, int64_t now_us);
  void set_speed(double new_speed, int64_t now_us);

 private:
  std::mutex mutex_;
  double speed_ = 1.0;
  bool paused_ = false;
  int64_t pts_us_ = 0;
  int64_t last_updated_us_ = 0;
  int64_t pause_pts_us_ = 0;
};
