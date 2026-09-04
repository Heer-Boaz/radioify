#pragma once

#include <chrono>
#include <cstdint>
#include <variant>

#include "playback/control/transport.h"
#include "playback/overlay/interaction.h"
#include "playback/session/context_menu_controller.h"
#include "playback/session/input_transport.h"
#include "playback/video/chapter/chapter.h"
#include "playback/video/edit/view.h"
#include "playback/video/timeline_preview_types.h"

namespace playback_session_input {

enum class CommandAction : std::uint8_t {
  RequestWindowPresent,
  RequestRedraw,
  RequestFrameRefresh,
  ToggleRadio,
  Toggle50Hz,
  CycleAudioTrack,
  ToggleSubtitles,
  ToggleChapterOverview,
  CloseChapterOverview,
  InstallChapterModel,
  CancelChapterOperation,
  RetryChapterAnalysis,
  ToggleWindowPresentation,
  TogglePictureInPicture,
  ToggleFullscreen,
  CopyCurrentVideoFrame,
  WaitForVideoEditExportAndExit,
  NavigateBack,
  ConfirmPendingExit,
  CancelPendingExit,
  CancelActiveMediaTask,
  ConfirmMediaAction,
  DismissMediaAction,
  ActivateSelectedMediaActionConfirmation,
  SelectPreviousMediaActionConfirmation,
  SelectNextMediaActionConfirmation,
};

struct ShowPlaybackControls {
  std::chrono::milliseconds duration{0};
};

struct SetOverlayControlHover {
  int token = -1;
};

struct SetPaused {
  bool paused = false;
};

struct SeekTo {
  int64_t targetUs = 0;
};

struct SeekBy {
  int64_t deltaUs = 0;
};

struct StepFrame {
  playback_video_frame_step::Direction direction =
      playback_video_frame_step::Direction::Next;
};

struct AdjustVolume {
  float delta = 0.0f;
};

struct TransportRequest {
  PlaybackTransportCommand command = PlaybackTransportCommand::Next;
};

struct ChapterNavigationRequest {
  playback_video_chapters::NavigationDirection direction =
      playback_video_chapters::NavigationDirection::Next;
};

// The chapter presentation view uses the same possibly edited timeline as
// playback. Overview hits therefore carry a real transport seek target.
struct SeekToChapter {
  int64_t timelineStartUs = 0;
};

struct SetChapterOverviewScroll {
  int offset = 0;
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
    std::variant<CommandAction, TransportRequest, ChapterNavigationRequest,
                 SeekToChapter, SetChapterOverviewScroll, VideoEditRequest,
                 ContextMenuRequest, MoveVideoEditBoundary, PlaybackExitRequest,
                 TimelinePreviewRequest, ClearTimelinePreview,
                 ShowPlaybackControls, SetOverlayControlHover, SetPaused,
                 SeekTo, SeekBy, StepFrame, AdjustVolume>;

struct SessionSnapshot {
  TransportSnapshot transport;
  double audioDurationSec = -1.0;
  bool audioSupports50HzToggle = false;
  bool pictureInPicture = false;
  bool videoEditorActive = false;
  playback_video_edit::Prompt videoEditPrompt =
      playback_video_edit::Prompt::None;
  bool mediaActionConfirmationPrompt = false;
  bool contextMenuVisible = false;
  bool chapterOverviewOpen = false;
  bool playbackControlsVisible = false;
  bool stopRequested = false;
};

struct InteractionRequest {
  playback_video_timeline_preview::PresentationSurface surface =
      playback_video_timeline_preview::PresentationSurface::Terminal;
  double x = 0.0;
  double y = 0.0;
  double scaleX = 1.0;
  double scaleY = 1.0;
  bool capturedProgress = false;
};

class SessionPort {
public:
  virtual ~SessionPort() = default;

  virtual bool dispatch(Command command) = 0;
  virtual SessionSnapshot snapshot() const = 0;
  virtual playback_overlay::InteractionHit
  hitTest(const InteractionRequest &request) const = 0;
};

} // namespace playback_session_input
