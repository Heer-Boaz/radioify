#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "playback/control/command.h"
#include "playback/control/transport.h"
#include "playback/input/shortcut_types.h"
#include "consoleinput.h"
#include "playback/ascii/frame_output.h"
#include "playback/session/context_menu_controller.h"
#include "playback/video/edit/view.h"
#include "playback/video/timeline_preview_types.h"
#include "state.h"

class Player;
class SubtitleManager;
class VideoWindow;
namespace playback_session {
class PlaybackOsdTimeline;
}

namespace playback_session_input {

inline bool isBackMousePressed(const MouseEvent& mouse) {
  constexpr DWORD kBackButtons = FROM_LEFT_2ND_BUTTON_PRESSED |
                                 FROM_LEFT_3RD_BUTTON_PRESSED |
                                 FROM_LEFT_4TH_BUTTON_PRESSED;
  return (mouse.buttonState & kBackButtons) != 0;
}

inline bool isVideoWindowFullscreenGesture(const MouseEvent& mouse) {
  return isWindowMouseEvent(mouse) &&
         (mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0 &&
         mouse.eventFlags == DOUBLE_CLICK;
}

struct PlaybackInputView {
  Player* player = nullptr;
  VideoWindow* videoWindow = nullptr;
  SubtitleManager* subtitleManager = nullptr;

  std::atomic<bool>* enableSubtitlesShared = nullptr;
  PlaybackSessionState* playbackState = nullptr;
  bool* audioOk = nullptr;
  bool hasSubtitles = false;

  playback_frame_output::FrameOutputState* frameOutputState = nullptr;

  playback_frame_output::LogLineWriter timingSink;
};

struct PlaybackInputSignals {
  std::atomic<int>* overlayControlHover = nullptr;
  std::function<void()> requestWindowPresent;
  std::function<bool()> toggleWindowPresentation;
  std::function<bool()> togglePictureInPicture;
  std::function<bool()> toggleFullscreen;
  std::function<bool(PlaybackTransportCommand)> requestTransportCommand;
  std::function<bool(const std::vector<std::filesystem::path>&)> requestOpenFiles;
  std::function<void()> copyCurrentVideoFrameToClipboard;
  std::function<bool()> videoEditorActive;
  std::function<playback_video_edit::Prompt()> videoEditPrompt;
  std::function<bool(playback_video_edit::Command)> executeVideoEditCommand;
  std::function<bool()> waitForVideoEditExportAndExit;
  std::function<void()> navigateBack;
  std::function<bool()> confirmPendingExit;
  std::function<bool()> cancelPendingExit;
  std::function<bool()> contextMenuVisible;
  std::function<bool(const playback_session::ContextMenuInput&)>
      handleContextMenuInput;
  std::function<bool(playback_video_edit::EditBoundary, int64_t timelineUs)>
      moveVideoEditBoundary;
  std::function<void(bool quitApplication)> requestPlaybackExit;
  std::function<void(playback_video_timeline_preview::PresentationSurface,
                     double ratio, int progressUnits)>
      requestTimelinePreview;
  std::function<void(playback_video_timeline_preview::PresentationSurface)>
      clearTimelinePreview;
  playback_session::PlaybackOsdTimeline* osd = nullptr;

  bool* loopStopRequested = nullptr;
  bool* redraw = nullptr;
  bool* forceRefreshArt = nullptr;
};

struct PlaybackSeekGestureState {
  struct VideoEditBoundaryDrag {
    playback_video_edit::EditBoundary boundary =
        playback_video_edit::EditBoundary::In;
    playback_video_timeline_preview::PresentationSurface surface =
        playback_video_timeline_preview::PresentationSurface::Terminal;
    int64_t targetTimelineUs = -1;
    uint64_t seekGenerationAtStart = 0;
  };

  struct PendingVideoEditBoundaryCommit {
    playback_video_edit::EditBoundary boundary =
        playback_video_edit::EditBoundary::In;
    uint64_t seekGeneration = 0;
  };

  std::chrono::steady_clock::time_point lastSeekSentTime =
      std::chrono::steady_clock::time_point::min();
  double queuedSeekTargetSec = -1.0;
  bool seekQueued = false;
  std::optional<playback_video_timeline_preview::PresentationSurface>
      progressDragSurface;
  std::optional<VideoEditBoundaryDrag> videoEditBoundaryDrag;
  std::optional<PendingVideoEditBoundaryCommit>
      pendingVideoEditBoundaryCommit;
};

enum class VideoEditBoundaryCommitState : uint8_t {
  Waiting,
  Ready,
  Superseded,
};

inline constexpr VideoEditBoundaryCommitState videoEditBoundaryCommitState(
    uint64_t expectedGeneration, uint64_t latestGeneration,
    uint64_t handledGeneration, bool seekPending, bool hasPresentedFrame) {
  if (expectedGeneration == 0 || latestGeneration > expectedGeneration) {
    return VideoEditBoundaryCommitState::Superseded;
  }
  if (latestGeneration < expectedGeneration ||
      handledGeneration < expectedGeneration || seekPending ||
      !hasPresentedFrame) {
    return VideoEditBoundaryCommitState::Waiting;
  }
  return VideoEditBoundaryCommitState::Ready;
}

inline constexpr int64_t videoEditBoundarySeekTargetUs(
    playback_video_edit::EditBoundary boundary, int64_t boundaryUs) {
  const int64_t clampedUs = boundaryUs > 0 ? boundaryUs : 0;
  // In is the first included frame. Out is an exclusive boundary displayed
  // as the final included frame, so resolve it from the instant immediately
  // before that boundary (including at sequence cuts).
  return boundary == playback_video_edit::EditBoundary::Out && clampedUs > 0
             ? clampedUs - 1
             : clampedUs;
}

bool isOverlayVisible(const PlaybackInputSignals& signals);
void setPlaybackPaused(const PlaybackInputView& view,
                       PlaybackInputSignals& signals,
                       PlaybackSeekGestureState& seekState, bool paused);
void queueSeekRequest(PlaybackInputSignals& signals,
                      PlaybackSeekGestureState& seekState, double targetSec);
void sendSeekRequest(const PlaybackInputView& view,
                     PlaybackInputSignals& signals,
                     PlaybackSeekGestureState& seekState, double targetSec);

void handlePlaybackInputEvent(const PlaybackInputView& view,
                              PlaybackInputSignals& signals,
                              PlaybackSeekGestureState& seekState,
                              const InputEvent& ev);
void handlePlaybackControlCommand(const PlaybackInputView& view,
                                  PlaybackInputSignals& signals,
                                  PlaybackSeekGestureState& seekState,
                                  PlaybackControlCommand command);
void handlePlaybackMouseEvent(const PlaybackInputView& view,
                              PlaybackInputSignals& signals,
                              PlaybackSeekGestureState& seekState,
                              const MouseEvent& mouse);
void handlePlaybackPointerLeave(PlaybackInputSignals& signals,
                                PlaybackSeekGestureState& seekState,
                                const PlaybackInputView& view);

}  // namespace playback_session_input
