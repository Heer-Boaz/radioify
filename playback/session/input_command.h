#pragma once

#include <cstdint>
#include <variant>

#include "playback/control/transport.h"
#include "playback/session/context_menu_controller.h"
#include "playback/video/edit/view.h"
#include "playback/video/timeline_preview_types.h"

namespace playback_session_input {

enum class CommandAction : std::uint8_t {
  RequestWindowPresent,
  ToggleWindowPresentation,
  TogglePictureInPicture,
  ToggleFullscreen,
  CopyCurrentVideoFrame,
  WaitForVideoEditExportAndExit,
  NavigateBack,
  ConfirmPendingExit,
  CancelPendingExit,
};

struct TransportRequest {
  PlaybackTransportCommand command = PlaybackTransportCommand::Next;
};

struct VideoEditRequest {
  playback_video_edit::Command command = playback_video_edit::Command::Open;
};

struct ContextMenuRequest {
  playback_session::ContextMenuInput input;
};

struct MoveVideoEditBoundary {
  playback_video_edit::EditBoundary boundary =
      playback_video_edit::EditBoundary::In;
  int64_t timelineUs = 0;
};

struct PlaybackExitRequest {
  bool quitApplication = false;
};

struct TimelinePreviewRequest {
  playback_video_timeline_preview::PresentationSurface surface =
      playback_video_timeline_preview::PresentationSurface::Terminal;
  double ratio = 0.0;
  int progressUnits = 0;
};

struct ClearTimelinePreview {
  playback_video_timeline_preview::PresentationSurface surface =
      playback_video_timeline_preview::PresentationSurface::Terminal;
};

using Command =
    std::variant<CommandAction, TransportRequest, VideoEditRequest,
                 ContextMenuRequest, MoveVideoEditBoundary,
                 PlaybackExitRequest, TimelinePreviewRequest,
                 ClearTimelinePreview>;

class CommandTarget {
 public:
  virtual ~CommandTarget() = default;

  virtual bool dispatch(Command command) = 0;
  virtual bool videoEditorActive() const = 0;
  virtual playback_video_edit::Prompt videoEditPrompt() const = 0;
  virtual bool contextMenuVisible() const = 0;
};

}  // namespace playback_session_input
