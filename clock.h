#pragma once

#include <atomic>
#include <cstdint>
#include <cmath>
#include <algorithm>

struct Clock {
  std::atomic<int32_t> speed_q16{65536};
  std::atomic<int64_t> pts_us{0};
  std::atomic<int64_t> pts_drift_us{0};
  std::atomic<int64_t> last_updated_us{0};
  std::atomic<int> serial{0};
  std::atomic<bool> paused{true};
  std::atomic<bool> valid{false};

  inline void reset(int newSerial) {
    serial.store(newSerial, std::memory_order_relaxed);
    pts_us.store(0, std::memory_order_relaxed);
    pts_drift_us.store(0, std::memory_order_relaxed);
    last_updated_us.store(0, std::memory_order_relaxed);
    paused.store(true, std::memory_order_relaxed);
    speed_q16.store(65536, std::memory_order_relaxed);
    valid.store(false, std::memory_order_relaxed);
  }

  inline void set(int64_t newPtsUs, int64_t nowUs, int newSerial) {
    serial.store(newSerial, std::memory_order_relaxed);
    pts_us.store(newPtsUs, std::memory_order_relaxed);
    last_updated_us.store(nowUs, std::memory_order_relaxed);
    const int32_t sp = speed_q16.load(std::memory_order_relaxed);
    const int64_t scaledNow =
        (sp == 65536) ? nowUs : (nowUs * static_cast<int64_t>(sp)) / 65536;
    pts_drift_us.store(newPtsUs - scaledNow, std::memory_order_relaxed);
    paused.store(false, std::memory_order_relaxed);
    valid.store(true, std::memory_order_relaxed);
  }

  // Task 2: Implement Clock Smoothing (Drift Management)
  // Instead of jumping PTS, we adjust the clock base weightedly or adjust speed.
  inline void sync_to_pts(int64_t targetPtsUs, int64_t nowUs, int newSerial) {
    if (paused.load(std::memory_order_relaxed)) {
        // If we were paused, this is a distinct resume event.
        // We must reset the drift anchor immediately because the previous drift
        // is from before the pause (and thus effectively ancient history).
        set(targetPtsUs, nowUs, newSerial);
        return;
    }

    if (serial.load(std::memory_order_relaxed) != newSerial || !valid.load(std::memory_order_relaxed)) {
        set(targetPtsUs, nowUs, newSerial);
        return;
    }

    int64_t currentPts = get(nowUs);
    int64_t diff = targetPtsUs - currentPts;

    // If drift is massive (> 1s), jump immediately to recover.
    if (std::abs(diff) > 1000000) {
        set(targetPtsUs, nowUs, newSerial);
        return;
    }
    
    // Average the drift to smooth out jitter from OS scheduling or callback timing.
    // We use a weighted moving average for the drift offset.
    const int32_t sp = speed_q16.load(std::memory_order_relaxed);
    const int64_t scaledNow = (sp == 65536) ? nowUs : (nowUs * static_cast<int64_t>(sp)) / 65536;
    int64_t newDrift = targetPtsUs - scaledNow;
    
    int64_t oldDrift = pts_drift_us.load(std::memory_order_relaxed);
    // 0.9 old + 0.1 new
    pts_drift_us.store((oldDrift * 9 + newDrift) / 10, std::memory_order_relaxed);
    last_updated_us.store(nowUs, std::memory_order_relaxed);
  }

  inline void set_paused(bool p, int64_t nowUs) {
    if (p) {
      const int64_t cur = get(nowUs);
      pts_us.store(cur, std::memory_order_relaxed);
      last_updated_us.store(nowUs, std::memory_order_relaxed);
      const int32_t sp = speed_q16.load(std::memory_order_relaxed);
      const int64_t scaledNow =
          (sp == 65536) ? nowUs : (nowUs * static_cast<int64_t>(sp)) / 65536;
      pts_drift_us.store(cur - scaledNow, std::memory_order_relaxed);
    }
    paused.store(p, std::memory_order_relaxed);
  }

  inline void set_speed(double speed, int64_t nowUs) {
    if (speed <= 0.0) {
      return;
    }
    int32_t next_q16 = static_cast<int32_t>(speed * 65536.0 + 0.5);
    if (next_q16 <= 0) {
      next_q16 = 1;
    }
    int32_t prev_q16 = speed_q16.load(std::memory_order_relaxed);
    if (next_q16 == prev_q16) {
      return;
    }
    bool was_valid = valid.load(std::memory_order_relaxed);
    bool was_paused = paused.load(std::memory_order_relaxed);
    int64_t cur = pts_us.load(std::memory_order_relaxed);
    if (was_valid && !was_paused) {
      const int64_t drift = pts_drift_us.load(std::memory_order_relaxed);
      const int64_t scaledNow =
          (prev_q16 == 65536)
              ? nowUs
              : (nowUs * static_cast<int64_t>(prev_q16)) / 65536;
      cur = drift + scaledNow;
    }
    speed_q16.store(next_q16, std::memory_order_relaxed);
    if (was_valid && !was_paused) {
      pts_us.store(cur, std::memory_order_relaxed);
      last_updated_us.store(nowUs, std::memory_order_relaxed);
      const int64_t scaledNow =
          (next_q16 == 65536)
              ? nowUs
              : (nowUs * static_cast<int64_t>(next_q16)) / 65536;
      pts_drift_us.store(cur - scaledNow, std::memory_order_relaxed);
    }
  }

  inline void invalidate() {
    valid.store(false, std::memory_order_relaxed);
    paused.store(true, std::memory_order_relaxed);
  }

  inline bool is_valid() const {
    return valid.load(std::memory_order_relaxed);
  }

  inline int64_t get(int64_t nowUs) const {
    if (!valid.load(std::memory_order_relaxed)) {
      return 0;
    }
    if (paused.load(std::memory_order_relaxed)) {
      return pts_us.load(std::memory_order_relaxed);
    }
    const int32_t sp = speed_q16.load(std::memory_order_relaxed);
    const int64_t drift = pts_drift_us.load(std::memory_order_relaxed);
    const int64_t scaledNow =
        (sp == 65536) ? nowUs : (nowUs * static_cast<int64_t>(sp)) / 65536;
    return drift + scaledNow;
  }
};
