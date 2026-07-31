#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "core/native_wait_handle.h"
#include "consolescreen.h"
#include "playback/video/gpu/gpu_shared.h"
#include "playback/video/framebuffer/frame_snapshot.h"
#include "playback/overlay/overlay.h"
#include "playback/session/state.h"
#include "playback/video/player.h"
#include "playback/video/framebuffer/window/window.h"

namespace playback_framebuffer_presenter {

class FrameSnapshotRequest {
 public:
  bool begin();
  bool pending() const;
  VideoFrameSnapshotResult wait();
  void complete(VideoFrameSnapshotResult result);
  void cancel(std::string error);
  void reset();

 private:
  enum class State {
    Idle,
    Pending,
    Completed,
  };

  mutable std::mutex mutex_;
  std::condition_variable completed_;
  State state_ = State::Idle;
  VideoFrameSnapshotResult result_;
};

using TextGridPresentationProvider =
    std::function<bool(int pixelWidth, int pixelHeight, int cellPixelWidth,
                       int cellPixelHeight,
                       const VideoFrame* frame, bool frameChanged,
                       const std::string& enhancementDebugLine,
                       std::vector<ScreenCell>& outCells,
                       int& outCols, int& outRows)>;

WindowUiState buildPlaybackFramebufferUiState(
    const std::string& windowTitle, VideoWindow& videoWindow, Player& player,
    SubtitleManager& subtitleManager, PlaybackSessionState playbackState,
    bool audioOk, bool canPlayPrevious, bool canPlayNext, bool hasSubtitles,
    std::atomic<bool>& enableSubtitlesShared,
    std::atomic<bool>& windowLocalSeekRequested,
    std::atomic<double>& windowPendingSeekTargetSec,
    std::atomic<int>& overlayControlHover,
    const playback_overlay::PlaybackOsdSnapshot& osd, bool debugOverlay);

void runFramebufferPresenterLoop(
    Player& player, VideoWindow& videoWindow, GpuVideoFrameCache& frameCache,
    std::atomic<WindowThreadState>& threadState,
    std::atomic<bool>& forcePresent, NativeWaitHandle wakeEvent,
    FrameSnapshotRequest& frameSnapshotRequest,
    const std::function<WindowUiState()>& buildUiState,
    const TextGridPresentationProvider& buildTextGridPresentation);

}  // namespace playback_framebuffer_presenter
