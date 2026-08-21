#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include "core/native_wait_handle.h"
#include "playback/control/command.h"
#include "playback/control/system_control_state.h"
#include "playback/control/transport.h"
#include "playback/session/presentation_policy.h"
#include "playback/session/state.h"

class ConsoleInput;
class ConsoleScreen;
struct Color;
struct InputEvent;
struct Style;
struct VideoPlaybackConfig;

enum class PlaybackSessionOpenOutcome {
  Ready,
  HandledWithoutPlayback,
  AudioFallbackRequested,
};

class PlaybackSession {
 public:
  struct Args {
    const std::filesystem::path& file;
    ConsoleInput& input;
    ConsoleScreen& screen;
    const Style& baseStyle;
    const Style& accentStyle;
    const Style& dimStyle;
    const Style& progressEmptyStyle;
    const Style& progressFrameStyle;
    const Color& progressStart;
    const Color& progressEnd;
    const VideoPlaybackConfig& config;
    bool* quitAppRequested = nullptr;
    std::function<bool(PlaybackTransportCommand)> requestTransportCommand;
    std::function<bool(const std::vector<std::filesystem::path>&)> requestOpenFiles;
    PlaybackSessionContinuationState* continuityState = nullptr;
    PlaybackSessionIntent sessionIntent = PlaybackSessionIntent::View;
  };

  explicit PlaybackSession(Args args);
  ~PlaybackSession();

  PlaybackSession(PlaybackSession&&) noexcept;
  PlaybackSession& operator=(PlaybackSession&&) noexcept;

  PlaybackSession(const PlaybackSession&) = delete;
  PlaybackSession& operator=(const PlaybackSession&) = delete;

  PlaybackSessionOpenOutcome open();
  bool pump();
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
