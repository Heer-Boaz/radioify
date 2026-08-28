#pragma once

#include <atomic>
#include <functional>
#include <utility>

namespace audio_separation {

// Cooperative scheduling control passed through the complete separation
// pipeline. A checkpoint may wait while foreground playback owns the GPU.
// Re-creatable accelerator resources can be yielded before that wait so a
// suspended background task does not keep VRAM reserved.
class ExecutionControl {
 public:
  using YieldResources = std::function<void()>;
  using Checkpoint =
      std::function<bool(const YieldResources& yieldResources)>;

  explicit ExecutionControl(const std::atomic<bool>* cancellationFlag,
                            Checkpoint checkpoint = {})
      : cancellationFlag_(cancellationFlag),
        checkpoint_(std::move(checkpoint)) {}

  bool cancellationRequested() const {
    return cancellationFlag_ &&
           cancellationFlag_->load(std::memory_order_relaxed);
  }

  bool checkpoint(const YieldResources& yieldResources = {}) const {
    if (cancellationRequested()) {
      return false;
    }
    return checkpoint_ ? checkpoint_(yieldResources)
                       : !cancellationRequested();
  }

  const std::atomic<bool>* cancellationFlag() const {
    return cancellationFlag_;
  }

 private:
  const std::atomic<bool>* cancellationFlag_ = nullptr;
  Checkpoint checkpoint_;
};

}  // namespace audio_separation
