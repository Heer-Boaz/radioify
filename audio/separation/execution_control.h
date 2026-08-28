#pragma once

#include <atomic>
#include <functional>
#include <utility>

namespace audio_separation {

// Cooperative control passed through the complete separation pipeline.
// checkpoint() may wait while foreground playback has resource priority and
// returns false as soon as cancellation is requested.
class ExecutionControl {
 public:
  using Checkpoint = std::function<bool()>;

  explicit ExecutionControl(const std::atomic<bool>* cancellationFlag,
                            Checkpoint checkpoint = {})
      : cancellationFlag_(cancellationFlag),
        checkpoint_(std::move(checkpoint)) {}

  bool cancellationRequested() const {
    return cancellationFlag_ &&
           cancellationFlag_->load(std::memory_order_relaxed);
  }

  bool checkpoint() const {
    if (cancellationRequested()) {
      return false;
    }
    return checkpoint_ ? checkpoint_() : !cancellationRequested();
  }

  const std::atomic<bool>* cancellationFlag() const {
    return cancellationFlag_;
  }

 private:
  const std::atomic<bool>* cancellationFlag_ = nullptr;
  Checkpoint checkpoint_;
};

}  // namespace audio_separation
