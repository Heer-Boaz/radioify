#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "playback/control/command.h"
#include "playback/control/transport.h"
#include "playback/input/shortcut_types.h"
#include "input_event.h"
#include "playback/ascii/frame_output.h"
#include "playback/session/input_command.h"
#include "playback/session/input_transport.h"
#include "state.h"

class SubtitleManager;
class VideoWindow;
namespace playback_session {
class PlaybackOsdTimeline;
}

namespace playback_session_input {

inline bool isPlaybackFullscreenGesture(const MouseEvent& mouse) {
  return mouse.button == MouseButton::Left &&
         mouse.kind == MouseEventKind::DoubleClick;
}

struct PlaybackInputView {
  Transport& transport;
  VideoWindow& videoWindow;
  SubtitleManager& subtitleManager;
  std::mutex& subtitleMutex;
  std::atomic<bool>& enableSubtitlesShared;
  bool hasSubtitles = false;
  playback_frame_output::FrameOutputState& frameOutputState;
  playback_frame_output::LogLineWriter timingSink;
};

struct PlaybackInputSignals {
  PlaybackInputSignals(CommandTarget& commandTarget,
                       std::atomic<int>& controlHover,
                       playback_session::PlaybackOsdTimeline& osdTimeline,
                       bool& stopRequested, bool& redrawRequested,
                       bool& forceArtRefresh)
      : commands(commandTarget),
        overlayControlHover(controlHover),
        osd(osdTimeline),
        loopStopRequested(stopRequested),
        redraw(redrawRequested),
        forceRefreshArt(forceArtRefresh) {}

  CommandTarget& commands;
  std::atomic<int>& overlayControlHover;
  playback_session::PlaybackOsdTimeline& osd;
  bool& loopStopRequested;
  bool& redraw;
  bool& forceRefreshArt;
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
