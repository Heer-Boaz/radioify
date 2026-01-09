#pragma once

#include <atomic>
#include <cstdint>

struct Clock {
  std::atomic<int32_t> speed_q16{65536};
  std::atomic<int64_t> pts_us{0};
  std::atomic<int64_t> pts_drift_us{0};
  std::atomic<int64_t> last_updated_us{0};
  std::atomic<int> serial{0};
  std::atomic<bool> paused{true};

  inline void reset(int newSerial) {
    serial.store(newSerial, std::memory_order_relaxed);
    pts_us.store(0, std::memory_order_relaxed);
    pts_drift_us.store(0, std::memory_order_relaxed);
    last_updated_us.store(0, std::memory_order_relaxed);
    paused.store(true, std::memory_order_relaxed);
    speed_q16.store(65536, std::memory_order_relaxed);
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

  inline int64_t get(int64_t nowUs) const {
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
