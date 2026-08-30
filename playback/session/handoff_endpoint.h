#pragma once

#include <optional>

#include "playback/session/exit_coordinator.h"

namespace playback_session {

// Minimal owner-thread port needed by the application shell to negotiate an
// external media handoff. It deliberately excludes playback and presentation
// controls so handoff state cannot retain or operate the full video session.
class HandoffEndpoint {
 public:
  virtual ~HandoffEndpoint() = default;

  virtual bool handoffRequestDeferred() const = 0;
  virtual std::optional<playback_session_exit::RequestId>
  requestHandoff() = 0;
  virtual bool resolveHandoff(
      playback_session_exit::RequestId requestId, bool accepted) = 0;
};

}  // namespace playback_session
