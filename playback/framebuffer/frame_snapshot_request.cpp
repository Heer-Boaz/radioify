#include "frame_snapshot_request.h"

#include <utility>

namespace playback_framebuffer_presenter {

bool FrameSnapshotRequest::begin() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_.load(std::memory_order_relaxed) != State::Idle) {
    return false;
  }
  result_ = {};
  state_.store(State::Pending, std::memory_order_release);
  return true;
}

bool FrameSnapshotRequest::pending() const {
  return state_.load(std::memory_order_acquire) == State::Pending;
}

VideoFrameSnapshotResult FrameSnapshotRequest::wait() {
  std::unique_lock<std::mutex> lock(mutex_);
  completed_.wait(lock, [this]() {
    return state_.load(std::memory_order_acquire) == State::Completed;
  });
  VideoFrameSnapshotResult result = std::move(result_);
  result_ = {};
  state_.store(State::Idle, std::memory_order_release);
  return result;
}

void FrameSnapshotRequest::complete(VideoFrameSnapshotResult result) {
  finish(std::move(result));
}

void FrameSnapshotRequest::cancel(std::string error) {
  VideoFrameSnapshotResult result;
  result.error = std::move(error);
  finish(std::move(result));
}

void FrameSnapshotRequest::finish(VideoFrameSnapshotResult result) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.load(std::memory_order_relaxed) != State::Pending) {
      return;
    }
    result_ = std::move(result);
    state_.store(State::Completed, std::memory_order_release);
  }
  completed_.notify_one();
}

}  // namespace playback_framebuffer_presenter
