#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/playback_queue.h"
#include "app/playback_route.h"
#include "core/native_wait_handle.h"
#include "core/open_file_requests.h"
#include "playback/control/command.h"
#include "playback/control/system_control_state.h"
#include "playback/media_processing_actions.h"
#include "playback/session/session.h"
#include "playback/target.h"

struct InputEvent;

namespace media_processing {
struct TaskCompletion;
}

// Owns media activation, playback-session handoff and image-viewer routing for
// the TUI. The browser loop submits intent and observes session state without
// owning the playback state machine itself.
class TuiMediaCoordinator {
 public:
  struct Callbacks {
    std::function<bool(const std::filesystem::path&, int)> startAudio;
    std::function<void(playback_route::AudioPictureInPicturePlan)>
        applyAudioPictureInPicturePlan;
    std::function<bool(const std::filesystem::path&)> openBrowserDirectory;
    std::function<void(std::string)> setCommandError;
    std::function<void()> requestQuit;
    std::function<void()> presentationFinished;
    std::function<void()> activateBrowserSurface;
  };

  struct Services {
    playback_queue::Queue& queue;
    PlaybackSession::Dependencies sessionDependencies;
    const VideoPlaybackConfig& videoConfig;
    OpenFileRequests& openFileRequests;
    playback_media_processing::Actions mediaProcessingActions;
    Callbacks callbacks;
  };

  explicit TuiMediaCoordinator(Services services);
  ~TuiMediaCoordinator();

  TuiMediaCoordinator(const TuiMediaCoordinator&) = delete;
  TuiMediaCoordinator& operator=(const TuiMediaCoordinator&) = delete;

  bool startPlayback(playback_route::Route route,
                     playback_queue::Source source);
  bool startFiles(playback_route::Route route,
                  const std::vector<std::filesystem::path>& files);
  bool startDroppedFiles(
      const std::vector<std::filesystem::path>& files,
      const WindowPlacementState* sourcePlacement,
      std::optional<PlaybackPresentationState> videoPresentation);
  bool openFiles(const OpenFilesRequest& request);
  bool pump();

  bool videoActive() const;
  PlaybackShellTerminalRole terminalRole() const;
  std::optional<PlaybackTarget> currentPlaybackTarget() const;
  std::vector<NativeWaitHandle> activityWaitHandles() const;
  int nextWakeTimeoutMs() const;
  std::optional<PlaybackControlState> playbackControlState() const;
  std::optional<PlaybackPresentationState> videoPresentationState() const;
  bool capturesBrowserInput() const;

  bool handleVideoInputEvent(const InputEvent& event);
  bool handleControlCommand(PlaybackControlCommand command);
  bool seekToRatio(double ratio);
  bool toggleWindowPresentation();
  bool togglePictureInPicture();
  bool toggleFullscreen();
  bool activateVideoPresentation();

  void handleMediaTaskCompletion(
      const media_processing::TaskCompletion& completion,
      std::string status);
  void requestQuit();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
