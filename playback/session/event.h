#pragma once

#include <variant>

#include "playback/session/exit_coordinator.h"

namespace playback_session {

struct Capabilities {
  bool transportHandoff = false;
  bool openFilesHandoff = false;
  bool browserSurfaceActivation = false;
};

struct BrowserSurfaceActivationRequested {};

using Event =
    std::variant<playback_session_exit::HandoffRequest,
                 playback_session_exit::HandoffCancellation,
                 BrowserSurfaceActivationRequested>;

}  // namespace playback_session
