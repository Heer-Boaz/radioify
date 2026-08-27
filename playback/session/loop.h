#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/native_wait_handle.h"
#include "core/wake_deadline.h"
#include "playback/control/command.h"
#include "playback/control/system_control_state.h"
#include "playback/control/transport.h"
#include "playback/media_processing_actions.h"
#include "playback/ascii/frame_output.h"
#include "playback/session/event.h"
#include "playback/session/state.h"
#include "log.h"
#include "playback/video/playback.h"

class ConsoleScreen;
class AudioPlaybackRuntime;
class GpuRuntime;
class Player;
class SubtitleManager;
struct Color;
struct InputEvent;
struct Style;

class PlaybackLoopRunner {
 public:
  struct Args {
    ConsoleScreen& screen;
    AudioPlaybackRuntime& audioPlayback;
    GpuRuntime& gpu;
    VideoPlaybackConfig config;
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
    bool subtitlesEnabled = false;
    std::string windowTitle;
    std::filesystem::path file;
    bool enableAudio;
    bool hasSubtitles = false;
    playback_session::Capabilities capabilities;
    playback_media_processing::Actions mediaProcessingActions;
    PlaybackSessionContinuationState continuityState;
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
  void shutdown();
  void renderFailureScreen();
  bool quitApplicationRequested() const;
  PlaybackSessionContinuationState continuationState() const;

  bool hasRenderFailure() const;
  const std::string& renderFailureMessage() const;
  const std::string& renderFailureDetail() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
