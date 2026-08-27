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

struct InputEvent;
namespace media_processing {
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
                             AudioPlaybackFailed, ShowImages, QuitRequested,
                             PresentationFinished, ActivateBrowserSurface,
                             OpenBrowserDirectory>;

  struct PollResult {
    bool playbackChanged = false;
    std::vector<Event> events;
  };

  struct Services {
    playback_queue::Queue& queue;
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
  PlaybackShellTerminalRole terminalRole() const;
  std::optional<VideoSnapshot> videoSnapshot() const;
  std::vector<NativeWaitHandle> waitHandles() const;
  wake_schedule::Deadline nextWakeDeadline() const;
  bool capturesBrowserInput() const;

  bool handleVideoInputEvent(const InputEvent& event);
  bool handleControlCommand(PlaybackControlCommand command);
  bool seekToRatio(double ratio);
  bool toggleWindowPresentation();
  bool togglePictureInPicture();
  bool toggleFullscreen();
  bool activateVideoPresentation();

  void requestQuit();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
