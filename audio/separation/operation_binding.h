#pragma once

#include <cassert>
#include <cstdint>
#include <string>
#include <utility>

#include "audio/separation/job.h"

namespace audio_separation {

enum class OperationAvailability : std::uint8_t {
  Ready,
  SetupRequired,
  Unavailable,
  Failed,
};

// Immutable composition result for one audio-separation implementation.
// A ready binding always owns executable work; every other state owns an
// actionable reason instead. This prevents an always-failing function from
// masquerading as an available backend.
class OperationBinding {
 public:
  static OperationBinding ready(Job::Operation operation,
                                std::string backendName) {
    assert(operation);
    assert(!backendName.empty());
    OperationBinding binding;
    if (!operation || backendName.empty()) {
      binding.availability_ = OperationAvailability::Failed;
      binding.detail_ =
          "The audio-separation backend was configured incorrectly.";
      return binding;
    }
    binding.availability_ = OperationAvailability::Ready;
    binding.operation_ = std::move(operation);
    binding.backendName_ = std::move(backendName);
    return binding;
  }

  static OperationBinding unavailable(OperationAvailability availability,
                                      std::string detail) {
    assert(availability != OperationAvailability::Ready);
    OperationBinding binding;
    binding.availability_ = availability == OperationAvailability::Ready
                                ? OperationAvailability::Failed
                                : availability;
    binding.detail_ = detail.empty()
                          ? "The audio-separation backend is unavailable."
                          : std::move(detail);
    return binding;
  }

  bool ready() const {
    return availability_ == OperationAvailability::Ready &&
           static_cast<bool>(operation_);
  }
  OperationAvailability availability() const { return availability_; }
  const std::string& backendName() const { return backendName_; }
  const std::string& detail() const { return detail_; }
  const Job::Operation& operation() const { return operation_; }

 private:
  OperationAvailability availability_ = OperationAvailability::Unavailable;
  Job::Operation operation_;
  std::string backendName_;
  std::string detail_;
};

}  // namespace audio_separation
