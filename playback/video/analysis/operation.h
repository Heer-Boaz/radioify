#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace playback_video_analysis {

struct OperationControl {
  std::function<bool()> cancelled;
  std::function<bool()> backgroundGpuAllowed;
  std::function<void(std::optional<double>, std::string)> progress;
};

enum class CapabilityState : std::uint8_t {
  Ready,
  SetupRequired,
  Unsupported,
  Yielded,
  Cancelled,
};

struct CapabilityResult {
  CapabilityState state = CapabilityState::Unsupported;
  std::string detail;
};

enum class OperationStatus : std::uint8_t {
  Succeeded,
  Yielded,
  Cancelled,
  Unsupported,
  Failed,
};

struct InstallResult {
  OperationStatus status = OperationStatus::Failed;
  std::string detail;
};

} // namespace playback_video_analysis
