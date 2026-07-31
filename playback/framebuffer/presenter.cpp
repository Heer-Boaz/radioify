#include "presenter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

#include "audioplayback.h"
#include "core/windows_message_pump.h"
#include "playback/debug/lines.h"
#include "playback/frame/refresh.h"
#include "playback/video/state/machine.h"
#include "video_pipeline.h"
#include "mini_player_tui.h"

namespace playback_framebuffer_presenter {
namespace {

constexpr auto kOverlayRefreshInterval = std::chrono::milliseconds(100);
constexpr auto kTextGridPresentationRefreshInterval =
    std::chrono::milliseconds(250);

void waitForPresenterWake(NativeWaitHandle wakeEvent) {
  if (wakeEvent) {
    waitForHandlesAndPumpThreadWindowMessages(1, &wakeEvent, INFINITE);
    return;
  }
  waitForHandlesAndPumpThreadWindowMessages(0, nullptr, 50);
}

void waitForPresenterActivity(NativeWaitHandle wakeEvent,
                              NativeWaitHandle frameEvent, int timeoutMs) {
  NativeWaitHandle handles[2];
  DWORD handleCount = 0;
  if (wakeEvent) {
    handles[handleCount++] = wakeEvent;
  }
  if (frameEvent) {
    handles[handleCount++] = frameEvent;
  }
  if (handleCount == 0) {
    const DWORD waitMs = timeoutMs < 0
                             ? INFINITE
                             : static_cast<DWORD>(std::max(0, timeoutMs));
    waitForHandlesAndPumpThreadWindowMessages(0, nullptr, waitMs);
    return;
  }
  const DWORD waitMs =
      timeoutMs < 0 ? INFINITE : static_cast<DWORD>(std::max(0, timeoutMs));
  waitForHandlesAndPumpThreadWindowMessages(handleCount, handles, waitMs);
}

}  // namespace

bool FrameSnapshotRequest::begin() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != State::Idle) {
    return false;
  }
  result_ = {};
  state_ = State::Pending;
  return true;
}

bool FrameSnapshotRequest::pending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ == State::Pending;
}

VideoFrameSnapshotResult FrameSnapshotRequest::wait() {
  std::unique_lock<std::mutex> lock(mutex_);
  completed_.wait(lock, [this]() { return state_ == State::Completed; });
  VideoFrameSnapshotResult result = std::move(result_);
  result_ = {};
  state_ = State::Idle;
  return result;
}

void FrameSnapshotRequest::complete(VideoFrameSnapshotResult result) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Pending) {
      return;
    }
    result_ = std::move(result);
    state_ = State::Completed;
  }
  completed_.notify_one();
}

void FrameSnapshotRequest::cancel(std::string error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Pending) {
      return;
    }
    result_ = {};
    result_.error = std::move(error);
    state_ = State::Completed;
  }
  completed_.notify_one();
}

void FrameSnapshotRequest::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  result_ = {};
  state_ = State::Idle;
}

WindowUiState buildPlaybackFramebufferUiState(
    const std::string& windowTitle, VideoWindow& videoWindow, Player& player,
    SubtitleManager& subtitleManager, PlaybackSessionState playbackState,
    bool audioOk, bool canPlayPrevious, bool canPlayNext, bool hasSubtitles,
    std::atomic<bool>& enableSubtitlesShared,
    std::atomic<bool>& windowLocalSeekRequested,
    std::atomic<double>& windowPendingSeekTargetSec,
    std::atomic<int>& overlayControlHover, bool overlayVisibleNow,
    bool debugOverlay) {
  int64_t clockUs = player.currentUs();
  double displaySec = 0.0;
  if (clockUs > 0) {
    displaySec = static_cast<double>(clockUs) / 1000000.0;
  }
  int64_t durUs = player.durationUs();
  double totalSec = -1.0;
  if (durUs > 0) {
    totalSec = static_cast<double>(durUs) / 1000000.0;
  } else if (audioOk) {
    totalSec = audioGetTotalSec();
  }
  if (totalSec > 0.0) {
    displaySec = std::clamp(displaySec, 0.0, totalSec);
  }
  const bool windowSeekRequested =
      windowLocalSeekRequested.load(std::memory_order_relaxed);
  bool seekingOverlay = player.isSeeking() || windowSeekRequested;
  const double windowPendingTargetSec =
      windowPendingSeekTargetSec.load(std::memory_order_relaxed);
  if (windowSeekRequested && totalSec > 0.0 && std::isfinite(totalSec) &&
      windowPendingTargetSec >= 0.0 &&
      std::isfinite(windowPendingTargetSec)) {
    displaySec = std::clamp(windowPendingTargetSec, 0.0, totalSec);
  }
  const bool subtitlesEnabledNow =
      enableSubtitlesShared.load(std::memory_order_relaxed);
  const bool playerTransportPaused =
      playback_video_state_machine::project(player.state()).transport ==
      playback_video_state_machine::TransportState::Paused;
  bool pausedNow =
      playbackState == PlaybackSessionState::Paused ||
      playbackState == PlaybackSessionState::Ended || player.isEnded() ||
      playerTransportPaused;
  bool audioFinishedNow = audioOk && audioIsFinished();

  playback_overlay::PlaybackOverlayInputs overlayInputs;
  overlayInputs.windowTitle = windowTitle;
  overlayInputs.audioOk = audioOk;
  overlayInputs.playPauseAvailable =
      playbackState == PlaybackSessionState::Active ||
      playbackState == PlaybackSessionState::Paused ||
      playbackState == PlaybackSessionState::Ended;
  overlayInputs.audioSupports50HzToggle = audioOk && audioSupports50HzToggle();
  overlayInputs.canPlayPrevious = canPlayPrevious;
  overlayInputs.canPlayNext = canPlayNext;
  overlayInputs.radioEnabled = audioIsRadioEnabled();
  overlayInputs.radioLabel = std::string(audioGetRadioFilterLabel());
  overlayInputs.hz50Enabled = audioIs50HzEnabled();
  overlayInputs.canCycleAudioTracks = audioOk && player.canCycleAudioTracks();
  overlayInputs.activeAudioTrackLabel =
      audioOk ? player.activeAudioTrackLabel() : "N/A";
  overlayInputs.subtitleManager = &subtitleManager;
  overlayInputs.hasSubtitles = hasSubtitles;
  overlayInputs.subtitlesEnabled = subtitlesEnabledNow;
  overlayInputs.subtitleClockUs = clockUs;
  overlayInputs.seekingOverlay = seekingOverlay;
  overlayInputs.displaySec = displaySec;
  overlayInputs.totalSec = totalSec;
  overlayInputs.volPct = static_cast<int>(std::round(audioGetVolume() * 100.0f));
  overlayInputs.overlayVisible = overlayVisibleNow;
  overlayInputs.paused = pausedNow;
  overlayInputs.audioFinished = audioFinishedNow;
  overlayInputs.pictureInPictureAvailable = videoWindow.IsOpen();
  overlayInputs.pictureInPictureActive =
      overlayInputs.pictureInPictureAvailable &&
      videoWindow.IsPictureInPicture();
  overlayInputs.subtitleRenderError = videoWindow.GetSubtitleRenderError();
  overlayInputs.screenWidth = 0;
  overlayInputs.screenHeight = 0;
  overlayInputs.windowWidth = videoWindow.IsOpen() ? videoWindow.GetWidth() : 0;
  overlayInputs.windowHeight =
      videoWindow.IsOpen() ? videoWindow.GetHeight() : 0;
  overlayInputs.artTop = 0;
  overlayInputs.progressBarX = 0;
  overlayInputs.progressBarY = 0;
  overlayInputs.progressBarWidth = 0;
  playback_overlay::PlaybackOverlayState overlayState =
      playback_overlay::buildPlaybackOverlayState(overlayInputs);
  WindowUiState ui = playback_overlay::buildWindowUiState(
      overlayState, overlayControlHover.load(std::memory_order_relaxed));
  if (debugOverlay) {
    ui.debugLines.push_back(videoWindow.OutputColorDebugLine());
    ui.debugLines.push_back(
        playback_debug_lines::videoFrameDebugLine(player.debugInfo()));
  }
  return ui;
}

void runFramebufferPresenterLoop(
    Player& player, VideoWindow& videoWindow, GpuVideoFrameCache& frameCache,
    std::atomic<WindowThreadState>& threadState,
    std::atomic<bool>& forcePresent, NativeWaitHandle wakeEvent,
    FrameSnapshotRequest& frameSnapshotRequest,
    const std::function<bool()>& uiRefreshActive,
    const std::function<WindowUiState()>& buildUiState,
    const TextGridPresentationProvider& buildTextGridPresentation) {
  if (!uiRefreshActive || !buildUiState) {
    return;
  }

  playback_frame_refresh::PlaybackFrameRefreshState frameRefresh;
  playback_framebuffer_video_pipeline::Pipeline videoPipeline;
  std::vector<ScreenCell> textGridPresentationCells;
  GpuTextGridFrame textGridPresentationFrame;
  auto lastOverlayPresent = std::chrono::steady_clock::time_point::min();
  bool lastWindowOverlayVisible = false;
  bool lastWindowTransientMessageVisible = false;
  bool lastWindowSeeking = false;
  auto lastTextGridPresentationPresent =
      std::chrono::steady_clock::time_point::min();
  int lastTextGridPresentationWidth = 0;
  int lastTextGridPresentationHeight = 0;
  int lastTextGridPresentationCellWidth = 0;
  int lastTextGridPresentationCellHeight = 0;
  const NativeWaitHandle frameEvent = player.videoFrameWaitHandle();
  while (threadState.load(std::memory_order_relaxed) !=
         WindowThreadState::Stopping) {
    videoWindow.PollEvents();
    if (threadState.load(std::memory_order_relaxed) ==
        WindowThreadState::Stopping) {
      break;
    }

    if (threadState.load(std::memory_order_relaxed) ==
        WindowThreadState::Disabled) {
      waitForPresenterWake(wakeEvent);
      continue;
    }

    if (!videoWindow.IsOpen() || !videoWindow.IsVisible()) {
      if (frameSnapshotRequest.pending()) {
        VideoFrameSnapshotResult unavailable;
        unavailable.error =
            "Frame capture is available while the video window is visible.";
        frameSnapshotRequest.complete(std::move(unavailable));
      }
      waitForPresenterWake(wakeEvent);
      continue;
    }

    const bool forcePresentRequested =
        forcePresent.load(std::memory_order_relaxed);
    const bool uiRefreshRequested = uiRefreshActive();
    const bool seekingRequested = player.isSeeking();
    const bool textGridPresentationRequested =
        videoWindow.IsTextGridPresentationEnabled();
    if (!forcePresentRequested) {
      int waitTimeoutMs = -1;
      auto tightenWaitTimeout = [&](int candidateMs) {
        if (candidateMs < 0) return;
        waitTimeoutMs = waitTimeoutMs < 0
                            ? candidateMs
                            : std::min(waitTimeoutMs, candidateMs);
      };
      if (uiRefreshRequested || seekingRequested ||
          lastWindowOverlayVisible || lastWindowTransientMessageVisible ||
          lastWindowSeeking) {
        const auto now = std::chrono::steady_clock::now();
        if (lastOverlayPresent == std::chrono::steady_clock::time_point::min()) {
          tightenWaitTimeout(0);
        } else {
          tightenWaitTimeout(std::max(
              0, static_cast<int>(std::chrono::duration_cast<
                                       std::chrono::milliseconds>(
                                       (lastOverlayPresent +
                                        kOverlayRefreshInterval) -
                                       now)
                                       .count())));
        }
      }
      if (textGridPresentationRequested) {
        const auto now = std::chrono::steady_clock::now();
        if (lastTextGridPresentationPresent ==
            std::chrono::steady_clock::time_point::min()) {
          tightenWaitTimeout(0);
        } else {
          tightenWaitTimeout(std::max(
              0, static_cast<int>(std::chrono::duration_cast<
                                       std::chrono::milliseconds>(
                                       (lastTextGridPresentationPresent +
                                        kTextGridPresentationRefreshInterval) -
                                       now)
                                       .count())));
        }
      }
      waitForPresenterActivity(wakeEvent, frameEvent, waitTimeoutMs);
      videoWindow.PollEvents();
    }

    if (threadState.load(std::memory_order_relaxed) ==
        WindowThreadState::Stopping) {
      break;
    }

    bool forcePresentNow =
        forcePresent.exchange(false, std::memory_order_relaxed);
    playback_frame_refresh::PlaybackFrameRefreshRequest frameRequest;
    frameRequest.forceRefresh = forcePresentNow;
    playback_frame_refresh::PlaybackFrameRefreshResult frameResult =
        playback_frame_refresh::refresh(player, frameRefresh, frameRequest);
    if (threadState.load(std::memory_order_relaxed) ==
        WindowThreadState::Stopping) {
      break;
    }
    const bool frameSnapshotPending = frameSnapshotRequest.pending();
    const bool textGridPresentationActive =
        videoWindow.IsTextGridPresentationEnabled();
    playback_framebuffer_video_pipeline::FrameRequest videoFrameRequest;
    videoFrameRequest.frame =
        frameResult.frameAvailable ? &frameRefresh.frame : nullptr;
    videoFrameRequest.frameCache = &frameCache;
    videoFrameRequest.targetWidth = videoWindow.GetWidth();
    videoFrameRequest.targetHeight = videoWindow.GetHeight();
    videoFrameRequest.frameChanged = frameResult.frameChanged;
    videoFrameRequest.forceRefresh = forcePresentNow;
    videoFrameRequest.textGridPresentationActive =
        textGridPresentationActive && !frameSnapshotPending;
    bool targetHdrOutput = videoWindow.OutputUsesHdr();
#if defined(RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO) && RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO
    if (frameResult.frameAvailable &&
        frameRefresh.frame.yuvTransfer == YuvTransfer::Sdr) {
      targetHdrOutput = true;
    }
#endif
    videoFrameRequest.targetHdrOutput = targetHdrOutput;
    playback_framebuffer_video_pipeline::FrameResult videoFrameResult =
        videoPipeline.process(videoFrameRequest);

    const VideoFrame* presentationFrame = videoFrameResult.frame;
    bool frameChanged = videoFrameResult.framebufferFrameChanged;
    bool textFrameChanged = videoFrameResult.textGridFrameChanged;

    bool seekingNow = player.isSeeking();
    WindowUiState ui;
    if (frameSnapshotPending || !textGridPresentationActive) {
      ui = buildUiState();
    }
    if (frameSnapshotPending) {
      VideoFrameSnapshotResult result =
          videoWindow.CaptureCurrentFrame(frameCache, ui);
      frameSnapshotRequest.complete(std::move(result));
    }
    if (textGridPresentationActive) {
      const int windowWidth = videoWindow.GetWidth();
      const int windowHeight = videoWindow.GetHeight();
      int cellWidth = 1;
      int cellHeight = 1;
      videoWindow.GetTextGridCellSize(cellWidth, cellHeight);
      const auto now = std::chrono::steady_clock::now();
      const bool refreshDue =
          lastTextGridPresentationPresent ==
              std::chrono::steady_clock::time_point::min() ||
          (now - lastTextGridPresentationPresent) >=
              kTextGridPresentationRefreshInterval;
      const bool sizeChanged = windowWidth != lastTextGridPresentationWidth ||
                               windowHeight != lastTextGridPresentationHeight ||
                               cellWidth !=
                                   lastTextGridPresentationCellWidth ||
                               cellHeight !=
                                   lastTextGridPresentationCellHeight;

      if (textFrameChanged || forcePresentNow || refreshDue || sizeChanged) {
        int textCols = 0;
        int textRows = 0;
        const VideoFrame* textFrame =
            videoFrameResult.frameAvailable ? presentationFrame : nullptr;
        if (buildTextGridPresentation &&
            buildTextGridPresentation(windowWidth, windowHeight, cellWidth,
                                          cellHeight, textFrame,
                                          textFrameChanged,
                                          videoFrameResult.debugLine,
                                          textGridPresentationCells, textCols,
                                          textRows)) {
          buildGpuTextGridFrameFromScreenCells(
              textGridPresentationCells, textCols, textRows,
              textGridPresentationFrame);
          videoWindow.PresentGpuTextGrid(textGridPresentationFrame);
        }
        lastTextGridPresentationPresent = std::chrono::steady_clock::now();
        lastTextGridPresentationWidth = windowWidth;
        lastTextGridPresentationHeight = windowHeight;
        lastTextGridPresentationCellWidth = cellWidth;
        lastTextGridPresentationCellHeight = cellHeight;
      }
      lastOverlayPresent = std::chrono::steady_clock::time_point::min();
      lastWindowOverlayVisible = false;
      lastWindowTransientMessageVisible = false;
      lastWindowSeeking = false;
      continue;
    }
    lastTextGridPresentationPresent =
        std::chrono::steady_clock::time_point::min();

    if (!ui.debugLines.empty() && !videoFrameResult.debugLine.empty()) {
      ui.debugLines.push_back(videoFrameResult.debugLine);
    }
    bool overlayVisibleNow = ui.overlayAlpha > 0.01f;
    const bool transientMessageVisibleNow = ui.transientMessage.has_value();
    bool overlayStateChanged = overlayVisibleNow != lastWindowOverlayVisible ||
                               transientMessageVisibleNow !=
                                   lastWindowTransientMessageVisible ||
                               seekingNow != lastWindowSeeking;
    bool overlayRefreshDue = false;
    if (overlayVisibleNow || transientMessageVisibleNow || seekingNow ||
        lastWindowOverlayVisible || lastWindowTransientMessageVisible ||
        lastWindowSeeking) {
      const auto now = std::chrono::steady_clock::now();
      overlayRefreshDue =
          lastOverlayPresent == std::chrono::steady_clock::time_point::min() ||
          (now - lastOverlayPresent) >= kOverlayRefreshInterval;
    }
    bool needsPresent = frameChanged || forcePresentNow || overlayRefreshDue ||
                        overlayStateChanged;

    if (needsPresent) {
      if (threadState.load(std::memory_order_relaxed) ==
          WindowThreadState::Stopping) {
        break;
      }
      videoWindow.WaitForFramePacing(std::chrono::milliseconds(16));
      if (threadState.load(std::memory_order_relaxed) ==
          WindowThreadState::Stopping) {
        break;
      }
      if (frameChanged) {
        videoWindow.Present(frameCache, ui);
      } else {
        videoWindow.PresentOverlay(frameCache, ui);
      }
      if (overlayVisibleNow || transientMessageVisibleNow || seekingNow) {
        lastOverlayPresent = std::chrono::steady_clock::now();
      }
      lastWindowOverlayVisible = overlayVisibleNow;
      lastWindowTransientMessageVisible = transientMessageVisibleNow;
      lastWindowSeeking = seekingNow;
    }
  }
}

}  // namespace playback_framebuffer_presenter
