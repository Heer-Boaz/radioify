#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "playback/control/command.h"
#include "playback/control/transport.h"
#include "playback/input/shortcut_types.h"
#include "input_event.h"
#include "playback/session/input_command.h"
#include "playback/session/input_transport.h"
#include "playback/session/overlay_control_pointer.h"
#include "state.h"

namespace playback_session_input {

inline bool isPlaybackFullscreenGesture(const MouseEvent& mouse) {
  return mouse.button == MouseButton::Left &&
         mouse.kind == MouseEventKind::DoubleClick;
}

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
  playback_session_pointer::State overlayControlPointer;
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

bool isOverlayVisible(const SessionPort& session);
void setPlaybackPaused(SessionPort& session,
                       PlaybackSeekGestureState& seekState, bool paused);
void queueSeekRequest(SessionPort& session,
                      PlaybackSeekGestureState& seekState, double targetSec);
void sendSeekRequest(SessionPort& session,
                     PlaybackSeekGestureState& seekState, double targetSec);
// Consume older gesture input before another owner issues a transport intent.
void commitQueuedSeek(SessionPort& session, PlaybackSeekGestureState& seekState);

void handlePlaybackInputEvent(SessionPort& session,
                              PlaybackSeekGestureState& seekState,
                              const InputEvent& ev);
void handlePlaybackControlCommand(SessionPort& session,
                                  PlaybackSeekGestureState& seekState,
                                  PlaybackControlCommand command);
void handlePlaybackMouseEvent(SessionPort& session,
                              PlaybackSeekGestureState& seekState,
                              const MouseEvent& mouse);
void handlePlaybackPointerLeave(SessionPort& session,
                                PlaybackSeekGestureState& seekState);

}  // namespace playback_session_input
