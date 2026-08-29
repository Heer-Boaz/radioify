#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <variant>

namespace audio_separation {

// Tagged outcomes for work that may be pre-empted by the scheduler.
// Interruption is not a backend failure: the owner must reach a checkpoint and
// may retry after the higher-priority lease has been released. Keeping failure
// detail inside the result prevents status, value, and error output parameters
// from describing contradictory states.
struct OperationSucceeded {};
struct OperationInterrupted {};
struct OperationFailure {
  std::string detail;
};

using ControlledOperationResult =
    std::variant<OperationSucceeded, OperationInterrupted, OperationFailure>;

template <typename Value>
using ControlledValueResult =
    std::variant<Value, OperationInterrupted, OperationFailure>;

// Cooperative scheduling control passed through the complete separation
// pipeline. A checkpoint may wait while foreground playback owns the GPU.
// Re-creatable accelerator resources can be yielded before that wait so a
// suspended background task does not keep VRAM reserved.
class ExecutionControl {
 public:
  using YieldResources = std::function<void()>;
  using Checkpoint =
      std::function<bool(const YieldResources& yieldResources)>;
  using Interrupt = std::function<void()>;
  using RegisterInterrupt =
      std::function<std::function<void()>(Interrupt interrupt)>;

  class InterruptionRegistration {
   public:
    InterruptionRegistration() = default;
    explicit InterruptionRegistration(std::function<void()> release)
        : release_(std::move(release)) {}
    ~InterruptionRegistration() { reset(); }

    InterruptionRegistration(InterruptionRegistration&& other) noexcept
        : release_(std::move(other.release_)) {
      other.release_ = {};
    }
    InterruptionRegistration& operator=(
        InterruptionRegistration&& other) noexcept {
      if (this == &other) return *this;
      reset();
      release_ = std::move(other.release_);
      other.release_ = {};
      return *this;
    }

    InterruptionRegistration(const InterruptionRegistration&) = delete;
    InterruptionRegistration& operator=(const InterruptionRegistration&) =
        delete;

    void reset() {
      if (!release_) return;
      std::function<void()> release = std::move(release_);
      release();
    }

   private:
    std::function<void()> release_;
  };

  explicit ExecutionControl(const std::atomic<bool>* cancellationFlag,
                            Checkpoint checkpoint = {},
                            RegisterInterrupt registerInterrupt = {})
      : cancellationFlag_(cancellationFlag),
        checkpoint_(std::move(checkpoint)),
        registerInterrupt_(std::move(registerInterrupt)) {}

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

  InterruptionRegistration registerInterruption(Interrupt interrupt) const {
    return InterruptionRegistration(
        registerInterrupt_ ? registerInterrupt_(std::move(interrupt))
                           : std::function<void()>{});
  }

 private:
  const std::atomic<bool>* cancellationFlag_ = nullptr;
  Checkpoint checkpoint_;
  RegisterInterrupt registerInterrupt_;
};

}  // namespace audio_separation
