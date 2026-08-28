#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "app/playback_queue.h"
#include "app/playback_route.h"
#include "core/native_wait_handle.h"
#include "core/open_file_requests.h"
#include "core/wake_deadline.h"
#include "playback/control/command.h"
#include "playback/control/system_control_state.h"
#include "playback/media_processing_actions.h"
#include "playback/session/session.h"
#include "playback/target.h"
#include "tui/image_viewer_sequence.h"
#include "tui/media_activation_decision.h"

struct InputEvent;
namespace media_processing {
class Coordinator;
struct TaskCompletion;
}

// Owns media activation, playback-session handoff and image-viewer routing for
// the TUI. The browser loop submits intent and observes session state without
// owning the playback state machine itself.
class TuiMediaCoordinator {
 public:
  struct VideoSnapshot {
    PlaybackControlState control;
    PlaybackPresentationState presentation;
  };

  struct ApplyAudioPictureInPicture {
    playback_route::AudioPictureInPicturePlan plan =
        playback_route::AudioPictureInPicturePlan::Keep;
  };
  struct CommandErrorChanged {
    std::string message;
  };
  struct AudioPlaybackFailed {
    std::filesystem::path file;
  };
  struct VideoPlaybackFailed {
    std::filesystem::path file;
    playback_session::Problem problem;
  };
  struct ShowImages {
    playback_route::AudioPictureInPicturePlan audioPictureInPicture =
        playback_route::AudioPictureInPicturePlan::Keep;
    image_viewer_sequence::Sequence sequence;
  };
  struct QuitRequested {};
  struct PresentationFinished {};
  struct ActivateBrowserSurface {};
  struct OpenBrowserDirectory {
    std::filesystem::path path;
  };

  using Event = std::variant<ApplyAudioPictureInPicture, CommandErrorChanged,
                             AudioPlaybackFailed, VideoPlaybackFailed,
                             tui_media_activation::AudioFallbackRequest,
                             ShowImages, QuitRequested, PresentationFinished,
                             ActivateBrowserSurface, OpenBrowserDirectory,
                             playback_session::MediaTaskCancellationRequested>;

  struct PollResult {
    bool playbackChanged = false;
    std::vector<Event> events;
  };

  struct Services {
    playback_queue::Queue& queue;
    media_processing::Coordinator& mediaProcessing;
    playback_media_processing::Actions mediaProcessingActions;
    PlaybackSession::Dependencies sessionDependencies;
    VideoPlaybackConfig videoConfig;
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
  PollResult poll();
  void handleMediaTaskCompletion(
      const media_processing::TaskCompletion& completion);

  bool videoActive() const;
  PlaybackControlSessionId controlSessionId() const;
  PlaybackShellTerminalRole terminalRole() const;
  std::optional<VideoSnapshot> videoSnapshot() const;
  std::vector<NativeWaitHandle> waitHandles() const;
  wake_schedule::Deadline nextWakeDeadline() const;
  bool capturesBrowserInput() const;
  bool canAcceptExternalMediaChange() const;
  void setExternalInputModal(bool modal);

  bool handleVideoInputEvent(const InputEvent& event);
  bool handleControlCommand(PlaybackControlCommand command);
  bool handleSystemControlCommand(
      const PlaybackControlCommandEvent& event);
  bool seekToRatio(double ratio);
  bool toggleWindowPresentation();
  bool togglePictureInPicture();
  bool toggleFullscreen();
  bool activateVideoPresentation();
  bool resolveAudioFallback(tui_media_activation::DecisionId decision,
                            bool playAudio);

  void requestQuit();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
