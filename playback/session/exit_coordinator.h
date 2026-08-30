#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <variant>
#include <vector>

#include "playback/control/transport.h"

namespace playback_session_exit {

using RequestId = std::uint64_t;

struct StopSession {};
struct QuitApplication {};
struct Transport {
  PlaybackTransportCommand command = PlaybackTransportCommand::Next;
};
struct OpenFiles {
  std::vector<std::filesystem::path> files;
};
struct ExternalHandoff {};

using Intent =
    std::variant<StopSession, QuitApplication, Transport, OpenFiles,
                 ExternalHandoff>;
using HandoffIntent = std::variant<Transport, OpenFiles, ExternalHandoff>;

struct HandoffRequest {
  RequestId id = 0;
  HandoffIntent intent;
};

struct HandoffCancellation {
  RequestId id = 0;
};

struct Transition {
  bool handled = false;
  bool finishSession = false;
  bool quitApplication = false;
  bool resumePlayback = false;
  std::optional<RequestId> requestId;
  std::optional<HandoffRequest> handoffRequest;
  std::optional<HandoffCancellation> handoffCancellation;
};

// Owns the complete leave-session protocol. Local exits complete immediately
// after optional confirmation; host-mediated exits remain pending until the
// shell resolves the matching request id.
class ExitCoordinator {
 public:
  Transition request(Intent intent, bool confirmationRequired,
                     bool playbackActive);
  Transition confirm();
  Transition cancel();
  Transition resolve(RequestId requestId, bool accepted);
  Transition abortHandoff(RequestId requestId);

  bool pending() const { return pending_.has_value(); }
  bool confirmationVisible() const;
  bool awaitingHandoffDecision() const;

 private:
  enum class Phase : std::uint8_t {
    AwaitingConfirmation,
    AwaitingHandoffDecision,
  };

  struct Pending {
    Intent intent;
    Phase phase = Phase::AwaitingConfirmation;
    std::optional<RequestId> requestId;
    bool resumePlaybackOnCancel = false;
  };

  static bool requiresHandoffDecision(const Intent& intent);
  static HandoffIntent toHandoffIntent(const Intent& intent);
  Transition requestHandoffDecision();
  Transition finishLocalExit();

  std::optional<Pending> pending_;
  RequestId nextRequestId_ = 1;
};

}  // namespace playback_session_exit
