#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <variant>

#include "app/playback_queue.h"
#include "audio/playback_session.h"
#include "playback/control/command.h"
#include "playback/control/system_control_state.h"
#include "playback/session/video_session.h"

namespace application_playback {

using VideoSessionRef =
    std::optional<std::reference_wrapper<playback_session::VideoSession>>;

struct ControlUnhandled {};
struct ControlApplied {};
struct VideoControlDispatched {
  bool handled = false;
  bool retireControlSession = false;
};
struct QueueTransportRequested {
  playback_queue::Direction direction = playback_queue::Direction::Next;
};

using ControlDispatch =
    std::variant<ControlUnhandled, ControlApplied, VideoControlDispatched,
                 QueueTransportRequested>;

// Owns control-session identity and routes transport to the currently active
// playback kind. Video-session lifetime remains with the application shell;
// callers provide an explicit reference for each owner-thread operation.
class PlaybackControlRouter {
 public:
  explicit PlaybackControlRouter(audio_playback::Session& audioPlayback);

  PlaybackControlSessionId beginSession();
  void endSession();
  [[nodiscard]] PlaybackControlSessionId sessionId() const noexcept;

  [[nodiscard]] std::optional<PlaybackControlState> bindVideoControlState(
      PlaybackControlState control) const;
  [[nodiscard]] ControlDispatch dispatch(
      PlaybackControlCommand command, VideoSessionRef videoSession);
  [[nodiscard]] ControlDispatch dispatch(
      const PlaybackControlCommandEvent& event,
      VideoSessionRef videoSession);
  bool seekToRatio(double ratio, VideoSessionRef videoSession);

 private:
  audio_playback::Session& audioPlayback_;
  std::uint64_t lastSessionValue_ = 0;
  PlaybackControlSessionId sessionId_;
};

}  // namespace application_playback
