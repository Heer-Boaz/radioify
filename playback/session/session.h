#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "core/native_wait_handle.h"
#include "playback/control/command.h"
#include "playback/control/system_control_state.h"
#include "playback/control/transport.h"
#include "playback/session/presentation_policy.h"
#include "playback/session/state.h"
#include "playback/video/playback.h"

class ConsoleInput;
class ConsoleScreen;
struct Color;
struct InputEvent;
struct Style;

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
  // Session-owned activation data; it remains valid across pump() calls.
  struct Request {
    std::filesystem::path file;
    VideoPlaybackConfig config;
    PlaybackSessionContinuationState continuityState;
    PlaybackSessionIntent sessionIntent = PlaybackSessionIntent::View;
    std::function<bool(PlaybackTransportCommand)> requestTransportCommand;
    std::function<bool(const std::vector<std::filesystem::path>&)>
        requestOpenFiles;
    std::function<bool()> mediaBackgroundTaskRunning;
    std::function<bool(const std::filesystem::path&)>
        requestIndexedTranscript;
    std::function<void()> activateBrowserSurface;
  };

  // These dependencies are borrowed and must outlive the session.
  struct Dependencies {
    ConsoleInput& input;
    ConsoleScreen& screen;
    const Style& baseStyle;
    const Style& accentStyle;
    const Style& dimStyle;
    const Style& progressEmptyStyle;
    const Style& progressFrameStyle;
    const Color& progressStart;
    const Color& progressEnd;
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
  int nextWakeTimeoutMs() const;
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
  bool requestHandoff(std::function<void(bool)> completion);
  void requestStop();
  void requestQuit();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
