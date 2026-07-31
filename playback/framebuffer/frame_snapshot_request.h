#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

#include "playback/video/framebuffer/frame_snapshot.h"

namespace playback_framebuffer_presenter {

class FrameSnapshotRequest {
 public:
  [[nodiscard]] bool begin();
  [[nodiscard]] bool pending() const;
  [[nodiscard]] VideoFrameSnapshotResult wait();
  void complete(VideoFrameSnapshotResult result);
  void cancel(std::string error);

 private:
  enum class State : uint8_t {
    Idle,
    Pending,
    Completed,
  };
  static_assert(std::atomic<State>::is_always_lock_free,
                "presenter request state must stay lock-free");

  void finish(VideoFrameSnapshotResult result);

  std::atomic<State> state_{State::Idle};
  std::mutex mutex_;
  std::condition_variable completed_;
  VideoFrameSnapshotResult result_;
};

}  // namespace playback_framebuffer_presenter
