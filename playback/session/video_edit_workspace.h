#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "playback/video/edit/command.h"
#include "playback/video/edit/view.h"

class Player;

namespace playback_video_timeline_preview {
class HoverModel;
class Provider;
}  // namespace playback_video_timeline_preview

namespace playback_session {

struct VideoEditActionResult {
  bool handled = false;
  bool pausePlayback = false;
  std::string message;
};

// Application-level owner of a video's edit document, export job, and live
// playback projection. Popup/OSD state and session-exit policy stay with the
// playback loop.
class VideoEditWorkspace {
 public:
  VideoEditWorkspace(
      std::filesystem::path sourcePath, Player& player,
      playback_video_timeline_preview::HoverModel& timelinePreview,
      playback_video_timeline_preview::Provider& timelinePreviewProvider);
  ~VideoEditWorkspace();

  VideoEditWorkspace(const VideoEditWorkspace&) = delete;
  VideoEditWorkspace& operator=(const VideoEditWorkspace&) = delete;

  bool active() const;
  bool hasUnexportedChanges() const;
  bool needsExitConfirmation() const;

  VideoEditActionResult execute(playback_video_edit::Command command);
  bool moveBoundary(playback_video_edit::EditBoundary boundary,
                    int64_t timelineUs);
  bool poll(std::string* message);
  void stop();

  playback_video_edit::EditSnapshot edit() const;
  playback_video_edit::ExportProgress exportProgress() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_session
