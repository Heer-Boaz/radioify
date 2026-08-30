#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "core/native_wait_handle.h"
#include "core/wake_deadline.h"
#include "playback/control/command.h"
#include "playback/control/system_control_state.h"
#include "playback/media_processing_actions.h"
#include "playback/session/event.h"
#include "playback/session/open_outcome.h"
#include "playback/session/presentation_policy.h"
#include "playback/session/state.h"
#include "playback/session/transition_state.h"
#include "playback/video/playback.h"

struct InputEvent;

enum class PlaybackSessionExitIntent {
  Stop,
  QuitApplication,
};

struct PlaybackSessionCompletion {
  PlaybackSessionExitIntent intent = PlaybackSessionExitIntent::Stop;
  PlaybackSessionContinuationState continuityState;
  std::optional<playback_session::Problem> failure;
};

namespace playback_session {

struct VideoSessionRequest {
  explicit VideoSessionRequest(
      playback_media_processing::Actions mediaProcessingActions)
      : mediaProcessingActions(std::move(mediaProcessingActions)) {}

  std::filesystem::path file;
  VideoPlaybackConfig config;
  PlaybackSessionContinuationState continuityState;
  PlaybackSessionIntent sessionIntent = PlaybackSessionIntent::View;
  Capabilities capabilities;
  playback_media_processing::Actions mediaProcessingActions;
};

// Application-facing protocol for one video playback session. The shell owns
// lifecycle and handoff; concrete decoding, rendering and windowing remain
// behind this boundary.
class VideoSession {
 public:
  virtual ~VideoSession() = default;

  virtual std::optional<OpenOutcome> startOpen() = 0;
  virtual std::optional<OpenOutcome> pumpOpen() = 0;
  virtual bool opening() const = 0;
  virtual bool ready() const = 0;
  virtual std::optional<TransitionSnapshot> transitionSnapshot() const = 0;
  virtual std::optional<PlaybackSessionCompletion> pump() = 0;
  virtual PlaybackShellTerminalRole terminalRole() const = 0;
  virtual std::vector<NativeWaitHandle> activityWaitHandles() const = 0;
  virtual wake_schedule::Deadline nextWakeDeadline() const = 0;
  virtual PlaybackControlState controlState() const = 0;
  virtual PlaybackPresentationState presentationState() const = 0;
  virtual bool capturesBrowserInput() const = 0;
  virtual void setExternalInputModal(bool modal) = 0;
  virtual bool handleInputEvent(const InputEvent& event) = 0;
  virtual bool pollWindowInput(InputEvent& event) = 0;
  virtual bool handleWindowInputEvent(const InputEvent& event) = 0;
  virtual bool handleControlCommand(PlaybackControlCommand command) = 0;
  virtual bool seekToRatio(double ratio) = 0;
  virtual bool toggleWindowPresentation() = 0;
  virtual bool togglePictureInPicture() = 0;
  virtual bool toggleFullscreen() = 0;
  virtual bool activatePresentation() = 0;
  virtual std::optional<playback_session_exit::RequestId> requestHandoff() = 0;
  virtual bool resolveHandoff(playback_session_exit::RequestId requestId,
                              bool accepted) = 0;
  virtual std::vector<Event> drainEvents() = 0;
  virtual void mediaTaskFinished(
      const playback_media_processing::Completion& completion) = 0;
  virtual void mediaTaskActivityChanged(
      std::optional<playback_media_processing::Activity> activity) = 0;
  virtual void requestStop() = 0;
  virtual void requestQuit() = 0;
};

using VideoSessionFactory =
    std::function<std::unique_ptr<VideoSession>(VideoSessionRequest request)>;

}  // namespace playback_session
