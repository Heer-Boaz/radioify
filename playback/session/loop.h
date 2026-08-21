#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/native_wait_handle.h"
#include "playback/control/command.h"
#include "playback/control/system_control_state.h"
#include "playback/control/transport.h"
#include "playback/ascii/frame_output.h"
#include "playback/session/state.h"
#include "log.h"
#include "playback/video/playback.h"

class ConsoleScreen;
class Player;
class SubtitleManager;
struct Color;
struct InputEvent;
struct Style;

class PlaybackLoopRunner {
 public:
  struct Args {
    ConsoleScreen& screen;
    const VideoPlaybackConfig& config;
    Player& player;
    SubtitleManager& subtitleManager;
    PerfLog& perfLog;
    const Style& baseStyle;
    const Style& accentStyle;
    const Style& dimStyle;
    const Style& progressEmptyStyle;
    const Style& progressFrameStyle;
    const Color& progressStart;
    const Color& progressEnd;
    playback_frame_output::LogLineWriter timingSink;
    playback_frame_output::LogLineWriter warningSink;
    std::atomic<bool>& enableSubtitlesShared;
    const std::string& windowTitle;
    const std::filesystem::path& file;
    bool enableAscii;
    bool enableAudio;
    bool hasSubtitles = false;
    bool* quitApplicationRequested = nullptr;
    std::function<bool(PlaybackTransportCommand)> requestTransportCommand;
    std::function<bool(const std::vector<std::filesystem::path>&)> requestOpenFiles;
    PlaybackSessionContinuationState* continuityState = nullptr;
    PlaybackSessionIntent sessionIntent = PlaybackSessionIntent::View;
  };

  explicit PlaybackLoopRunner(Args args);
  ~PlaybackLoopRunner();

  PlaybackLoopRunner(PlaybackLoopRunner&&) noexcept;
  PlaybackLoopRunner& operator=(PlaybackLoopRunner&&) noexcept;

  PlaybackLoopRunner(const PlaybackLoopRunner&) = delete;
  PlaybackLoopRunner& operator=(const PlaybackLoopRunner&) = delete;

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
  void shutdown();
  void renderFailureScreen();
  PlaybackSessionContinuationState continuationState() const;

  bool hasRenderFailure() const;
  const std::string& renderFailureMessage() const;
  const std::string& renderFailureDetail() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
