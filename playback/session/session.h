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
#include "playback/session/open_outcome.h"
#include "playback/session/transition_state.h"
#include "playback/session/presentation_policy.h"
#include "playback/session/state.h"
#include "playback/video/playback.h"
#include "tui/style.h"

class ConsoleScreen;
class AudioPlaybackRuntime;
class GpuRuntime;
struct InputEvent;
namespace playback_session {
class SubtitleLoadService;
}

enum class PlaybackSessionExitIntent {
  Stop,
  QuitApplication,
};

struct PlaybackSessionCompletion {
  PlaybackSessionExitIntent intent = PlaybackSessionExitIntent::Stop;
  PlaybackSessionContinuationState continuityState;
  std::optional<playback_session::Problem> failure;
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

  // Process services and the screen are borrowed and must outlive the
  // session. Appearance is copied so presentation state has no separate
  // lifetime contract.
  struct Dependencies {
    AudioPlaybackRuntime& audioPlayback;
    GpuRuntime& gpu;
    ConsoleScreen& screen;
    playback_session::SubtitleLoadService& subtitleLoader;
    Appearance appearance;
  };

  PlaybackSession(Request request, Dependencies dependencies);
  ~PlaybackSession();

  PlaybackSession(PlaybackSession&&) noexcept;
  PlaybackSession& operator=(PlaybackSession&&) noexcept;

  PlaybackSession(const PlaybackSession&) = delete;
  PlaybackSession& operator=(const PlaybackSession&) = delete;

  // Opening can be asynchronous. An empty startOpen() result keeps the session
  // alive but not controllable; the owner drives pumpOpen(), wait handles and
  // deadlines until a terminal open outcome is returned.
  std::optional<playback_session::OpenOutcome> startOpen();
  std::optional<playback_session::OpenOutcome> pumpOpen();
  bool opening() const;
  bool ready() const;
  std::optional<playback_session::TransitionSnapshot> transitionSnapshot()
      const;
  std::optional<PlaybackSessionCompletion> pump();
  PlaybackShellTerminalRole terminalRole() const;
  std::vector<NativeWaitHandle> activityWaitHandles() const;
  wake_schedule::Deadline nextWakeDeadline() const;
  PlaybackControlState controlState() const;
  PlaybackPresentationState presentationState() const;
  bool capturesBrowserInput() const;
  void setExternalInputModal(bool modal);
  bool handleInputEvent(const InputEvent& event);
  bool pollWindowInput(InputEvent& event);
  bool handleWindowInputEvent(const InputEvent& event);
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
  void mediaTaskActivityChanged(
      std::optional<playback_media_processing::Activity> activity);
  void requestStop();
  void requestQuit();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
