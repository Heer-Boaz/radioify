#pragma once

#include <variant>

#include "playback/media_processing_actions.h"
#include "playback/session/exit_coordinator.h"

namespace playback_session {

struct Capabilities {
  bool transportHandoff = false;
  bool openFilesHandoff = false;
  bool browserSurfaceActivation = false;
};

struct BrowserSurfaceActivationRequested {};

struct MediaTaskCancellationRequested {
  playback_media_processing::CancellationRequest request;
};

using Event =
    std::variant<playback_session_exit::HandoffRequest,
                 playback_session_exit::HandoffCancellation,
                 BrowserSurfaceActivationRequested,
                 MediaTaskCancellationRequested>;

}  // namespace playback_session
