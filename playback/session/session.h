#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "core/native_wait_handle.h"
#include "core/wake_deadline.h"
#include "playback/control/command.h"
#include "playback/control/system_control_state.h"
#include "playback/control/transport.h"
#include "playback/media_processing_actions.h"
#include "playback/session/event.h"
#include "playback/session/presentation_policy.h"
#include "playback/session/state.h"
#include "playback/video/playback.h"
#include "tui/style.h"

class ConsoleInput;
class ConsoleScreen;
class AudioPlaybackRuntime;
struct InputEvent;

enum class PlaybackSessionOpenOutcome {
  Ready,
  HandledWithoutPlayback,
  AudioFallbackRequested,
  QuitApplicationRequested,
};

enum class PlaybackSessionExitIntent {
  Stop,
  QuitApplication,
};

struct PlaybackSessionCompletion {
  PlaybackSessionExitIntent intent = PlaybackSessionExitIntent::Stop;
  PlaybackSessionContinuationState continuityState;
};

class PlaybackSession {
 public:
  struct Appearance {
    Style baseStyle;
    Style accentStyle;
    Style dimStyle;
    Style progressEmptyStyle;
    Style progressFrameStyle;
    Color progressStart;
    Color progressEnd;
  };

  // Session-owned activation data; it remains valid across pump() calls.
  struct Request {
    explicit Request(
        playback_media_processing::Actions mediaProcessingActions)
        : mediaProcessingActions(std::move(mediaProcessingActions)) {}

    std::filesystem::path file;
    VideoPlaybackConfig config;
    PlaybackSessionContinuationState continuityState;
    PlaybackSessionIntent sessionIntent = PlaybackSessionIntent::View;
    playback_session::Capabilities capabilities;
    playback_media_processing::Actions mediaProcessingActions;
  };

  // The process audio owner, input, and screen are borrowed and must outlive
  // the session. Appearance is copied so presentation state has no separate
  // lifetime contract.
  struct Dependencies {
    AudioPlaybackRuntime& audioPlayback;
    ConsoleInput& input;
    ConsoleScreen& screen;
    Appearance appearance;
  };

  PlaybackSession(Request request, Dependencies dependencies);
  ~PlaybackSession();

  PlaybackSession(PlaybackSession&&) noexcept;
  PlaybackSession& operator=(PlaybackSession&&) noexcept;

  PlaybackSession(const PlaybackSession&) = delete;
  PlaybackSession& operator=(const PlaybackSession&) = delete;

  PlaybackSessionOpenOutcome open();
  std::optional<PlaybackSessionCompletion> pump();
  PlaybackShellTerminalRole terminalRole() const;
  std::vector<NativeWaitHandle> activityWaitHandles() const;
  wake_schedule::Deadline nextWakeDeadline() const;
  PlaybackControlState controlState() const;
  PlaybackPresentationState presentationState() const;
  bool capturesBrowserInput() const;
  bool handleInputEvent(const InputEvent& event);
  bool handleControlCommand(PlaybackControlCommand command);
  bool seekToRatio(double ratio);
  bool toggleWindowPresentation();
  bool togglePictureInPicture();
  bool toggleFullscreen();
  bool activatePresentation();
  std::optional<playback_session_exit::RequestId> requestHandoff();
  bool resolveHandoff(playback_session_exit::RequestId requestId,
                      bool accepted);
  std::vector<playback_session::Event> drainEvents();
  void mediaTaskFinished(
      const playback_media_processing::Completion& completion);
  void requestStop();
  void requestQuit();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
