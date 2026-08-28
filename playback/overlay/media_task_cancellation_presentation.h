#pragma once

#include <cstdint>
#include <string>

namespace playback_overlay {

enum class MediaTaskCancellationSelection : std::uint8_t {
  CancelTask,
  KeepRunning,
};

// Presentation-only projection of the session-owned cancellation decision.
// Render backends intentionally receive no task identity, source path or
// cancellation capability: those remain workflow concerns.
struct MediaTaskCancellationDialog {
  std::string title;
  std::string sourceName;
  MediaTaskCancellationSelection selected =
      MediaTaskCancellationSelection::KeepRunning;
};

}  // namespace playback_overlay
