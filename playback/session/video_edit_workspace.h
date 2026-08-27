#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/native_wait_handle.h"
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
  bool exportStarted = false;
};

enum class VideoEditExportCompletion : uint8_t {
  None,
  Succeeded,
  Failed,
  Cancelled,
};

struct VideoEditPollResult {
  bool changed = false;
  VideoEditExportCompletion completion = VideoEditExportCompletion::None;
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
  playback_video_edit::Prompt prompt() const;
  bool hasUnexportedChanges() const;
  bool needsExitConfirmation() const;
  playback_video_edit::ExitContext exitContext() const;

  VideoEditActionResult execute(playback_video_edit::Command command);
  VideoEditActionResult navigateBack();
  bool selectCutAt(int64_t timelineUs, int64_t toleranceUs);
  bool selectSceneSuggestionAt(int64_t timelineUs, int64_t toleranceUs);
  void clearCutSelection();
  bool moveBoundary(playback_video_edit::EditBoundary boundary,
                    int64_t timelineUs);
  VideoEditPollResult poll();
  std::vector<NativeWaitHandle> waitHandles() const;
  void stop();

  playback_video_edit::EditSnapshot edit() const;
  playback_video_edit::ExportProgress exportProgress() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_session
