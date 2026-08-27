#include "presenter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

#include "core/thread_dispatch_queue.h"
#include "core/windows_message_pump.h"
#include "playback/debug/lines.h"
#include "playback/frame/refresh.h"
#include "playback/session/state.h"
#include "playback/video/state/machine.h"
#include "video_pipeline.h"
#include "mini_player_tui.h"

namespace playback_framebuffer_presenter {
namespace {

constexpr auto kSeekingRefreshInterval = std::chrono::milliseconds(100);
constexpr auto kTextGridPresentationRefreshInterval =
    std::chrono::milliseconds(250);

void waitForPresenterWake(NativeWaitHandle wakeEvent,
                          NativeWaitHandle dispatchEvent) {
  NativeWaitHandle handles[2];
  DWORD handleCount = 0;
  if (wakeEvent) {
    handles[handleCount++] = wakeEvent;
  }
  if (dispatchEvent) {
    handles[handleCount++] = dispatchEvent;
  }
  waitForHandlesAndPumpThreadWindowMessages(
      handleCount, handleCount > 0 ? handles : nullptr, std::nullopt);
}

void waitForPresenterActivity(NativeWaitHandle wakeEvent,
                              NativeWaitHandle frameEvent,
                              NativeWaitHandle dispatchEvent,
                              wake_schedule::Deadline deadline) {
  NativeWaitHandle handles[3];
  DWORD handleCount = 0;
  if (wakeEvent) {
    handles[handleCount++] = wakeEvent;
  }
  if (frameEvent) {
    handles[handleCount++] = frameEvent;
  }
  if (dispatchEvent) {
    handles[handleCount++] = dispatchEvent;
  }
  waitForHandlesAndPumpThreadWindowMessages(
      handleCount, handleCount > 0 ? handles : nullptr, deadline);
}

}  // namespace

WindowUiState buildPlaybackFramebufferUiState(
    const std::string& windowTitle, VideoWindow& videoWindow, Player& player,
    SubtitleManager& subtitleManager, const AudioPlaybackSnapshot& audio,
    PlaybackSessionState playbackState, bool audioOk,
    bool canPlayPrevious, bool canPlayNext, bool hasSubtitles,
    std::atomic<bool>& enableSubtitlesShared,
    std::atomic<int>& overlayControlHover,
    const PlaybackFramebufferUiSnapshot& snapshot, bool debugOverlay) {
  const PlayerTimelineSnapshot timeline = player.timelineSnapshot();
  const int64_t clockUs = timeline.positionUs;
  double displaySec = 0.0;
  if (clockUs > 0) {
    displaySec = static_cast<double>(clockUs) / 1000000.0;
  }
  int64_t durUs = player.durationUs();
  double totalSec = -1.0;
  if (durUs > 0) {
    totalSec = static_cast<double>(durUs) / 1000000.0;
  } else if (audioOk) {
    totalSec = audio.durationSec;
  }
  if (totalSec > 0.0) {
    displaySec = std::clamp(displaySec, 0.0, totalSec);
  }
  const bool seekingOverlay = timeline.seekPending();
  const bool subtitlesEnabledNow =
      enableSubtitlesShared.load(std::memory_order_relaxed);
  const bool playerTransportPaused =
      playback_video_state_machine::project(player.state()).transport ==
      playback_video_state_machine::TransportState::Paused;
  bool pausedNow =
      playbackState == PlaybackSessionState::Paused ||
      playbackState == PlaybackSessionState::Ended || player.isEnded() ||
      playerTransportPaused;

  playback_overlay::PlaybackOverlayInputs overlayInputs;
  overlayInputs.windowTitle = windowTitle;
  overlayInputs.audioOk = audioOk;
  overlayInputs.playPauseAvailable =
      playbackState == PlaybackSessionState::Active ||
      playbackState == PlaybackSessionState::Paused ||
      playbackState == PlaybackSessionState::Ended;
  overlayInputs.audioSupports50HzToggle =
      audioOk && audio.supports50HzToggle;
  overlayInputs.canPlayPrevious = canPlayPrevious;
  overlayInputs.canPlayNext = canPlayNext;
  overlayInputs.radioEnabled = audio.radioEnabled;
  overlayInputs.radioLabel = std::string(audio.radioFilterLabel);
  overlayInputs.hz50Enabled = audio.hz50Enabled;
  overlayInputs.canCycleAudioTracks = audioOk && player.canCycleAudioTracks();
  overlayInputs.activeAudioTrackLabel =
      audioOk ? player.activeAudioTrackLabel() : "N/A";
  overlayInputs.subtitleManager = &subtitleManager;
  overlayInputs.hasSubtitles = hasSubtitles;
  overlayInputs.subtitlesEnabled = subtitlesEnabledNow;
  overlayInputs.subtitleClockUs = timeline.sourcePositionUs;
  overlayInputs.seekingOverlay = seekingOverlay;
  overlayInputs.displaySec = displaySec;
  overlayInputs.totalSec = totalSec;
  overlayInputs.volPct =
      static_cast<int>(std::round(audio.volume * 100.0f));
  overlayInputs.osd = snapshot.osd;
  overlayInputs.paused = pausedNow;
  overlayInputs.pictureInPictureAvailable = videoWindow.IsOpen();
  overlayInputs.pictureInPictureActive =
      overlayInputs.pictureInPictureAvailable &&
      videoWindow.IsPictureInPicture();
  overlayInputs.subtitleRenderError = videoWindow.GetSubtitleRenderError();
  overlayInputs.contextMenu = snapshot.contextMenu;
  overlayInputs.videoEdit = snapshot.videoEdit;
  if (overlayInputs.videoEdit.active) {
    overlayInputs.videoEdit.playheadTimelineUs = timeline.positionUs;
  }
  overlayInputs.videoEditExport = snapshot.videoEditExport;
  overlayInputs.videoEditPrompt = snapshot.videoEditPrompt;
  playback_overlay::PlaybackOverlayState overlayState =
      playback_overlay::buildPlaybackOverlayState(overlayInputs);
  WindowUiState ui = playback_overlay::buildWindowUiState(
      overlayState, overlayControlHover.load(std::memory_order_relaxed));
  ui.timelinePreview = snapshot.timelinePreview;
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
    ThreadDispatchQueue& dispatch, PresentationSource& presentationSource) {
  playback_frame_refresh::PlaybackFrameRefreshState frameRefresh;
  playback_framebuffer_video_pipeline::Pipeline videoPipeline;
  std::vector<ScreenCell> textGridPresentationCells;
  GpuTextGridFrame textGridPresentationFrame;
  auto lastSeekingPresent = std::chrono::steady_clock::time_point::min();
  bool lastWindowSeeking = false;
  auto lastTextGridPresentationPresent =
      std::chrono::steady_clock::time_point::min();
  int lastTextGridPresentationWidth = 0;
  int lastTextGridPresentationHeight = 0;
  int lastTextGridPresentationCellWidth = 0;
  int lastTextGridPresentationCellHeight = 0;
  const NativeWaitHandle frameEvent = player.videoFrameWaitHandle();
  const NativeWaitHandle dispatchEvent = dispatch.nativeWaitHandle();
  while (threadState.load(std::memory_order_relaxed) !=
         WindowThreadState::Stopping) {
    videoWindow.PollEvents();
    dispatch.processPending();
    if (threadState.load(std::memory_order_relaxed) ==
        WindowThreadState::Stopping) {
      break;
    }

    if (threadState.load(std::memory_order_relaxed) ==
        WindowThreadState::Disabled) {
      waitForPresenterWake(wakeEvent, dispatchEvent);
      continue;
    }

    if (!videoWindow.IsOpen() || !videoWindow.IsVisible()) {
      waitForPresenterWake(wakeEvent, dispatchEvent);
      continue;
    }

    const bool forcePresentRequested =
        forcePresent.load(std::memory_order_relaxed);
    const bool seekingRequested = player.timelineSnapshot().seekPending();
    const bool textGridPresentationRequested =
        videoWindow.IsTextGridPresentationEnabled();
    if (!forcePresentRequested) {
      wake_schedule::Deadline waitDeadline;
      const auto now = wake_schedule::Clock::now();
      if (seekingRequested || lastWindowSeeking) {
        if (lastSeekingPresent ==
            std::chrono::steady_clock::time_point::min()) {
          wake_schedule::include(waitDeadline, now);
        } else {
          wake_schedule::include(waitDeadline,
                                 lastSeekingPresent + kSeekingRefreshInterval);
        }
      }
      if (textGridPresentationRequested) {
        if (lastTextGridPresentationPresent ==
            std::chrono::steady_clock::time_point::min()) {
          wake_schedule::include(waitDeadline, now);
        } else {
          wake_schedule::include(
              waitDeadline, lastTextGridPresentationPresent +
                                kTextGridPresentationRefreshInterval);
        }
      }
      waitForPresenterActivity(wakeEvent, frameEvent, dispatchEvent,
                               waitDeadline);
      videoWindow.PollEvents();
      dispatch.processPending();
    }

    if (threadState.load(std::memory_order_relaxed) ==
        WindowThreadState::Stopping) {
      break;
    }

    const bool forcePresentNow =
        forcePresent.exchange(false, std::memory_order_relaxed);
    playback_frame_refresh::PlaybackFrameRefreshRequest frameRequest;
    frameRequest.forceRefresh = forcePresentNow;
    playback_frame_refresh::PlaybackFrameRefreshResult frameResult =
        playback_frame_refresh::refresh(player, frameRefresh, frameRequest);
    if (threadState.load(std::memory_order_relaxed) ==
        WindowThreadState::Stopping) {
      break;
    }
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
    videoFrameRequest.textGridPresentationActive = textGridPresentationActive;
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

    const bool seekingNow = player.timelineSnapshot().seekPending();
    WindowUiState ui;
    if (!textGridPresentationActive) {
      ui = presentationSource.buildWindowUiState(videoWindow);
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
        playback_overlay::InteractionMap textInteractions;
        const VideoFrame* textFrame =
            videoFrameResult.frameAvailable ? presentationFrame : nullptr;
        TextGridPresentationRequest request{
            videoWindow, windowWidth, windowHeight, cellWidth, cellHeight,
            textFrame, textFrameChanged, videoFrameResult.debugLine};
        TextGridPresentationTarget target{
            textGridPresentationCells, textCols, textRows, textInteractions};
        if (presentationSource.renderTextGrid(request, target)) {
          buildGpuTextGridFrameFromScreenCells(
              textGridPresentationCells, textCols, textRows,
              textGridPresentationFrame);
          videoWindow.PresentGpuTextGrid(textGridPresentationFrame,
                                         textInteractions);
        }
        lastTextGridPresentationPresent = std::chrono::steady_clock::now();
        lastTextGridPresentationWidth = windowWidth;
        lastTextGridPresentationHeight = windowHeight;
        lastTextGridPresentationCellWidth = cellWidth;
        lastTextGridPresentationCellHeight = cellHeight;
      }
      lastSeekingPresent = std::chrono::steady_clock::time_point::min();
      lastWindowSeeking = false;
      continue;
    }
    lastTextGridPresentationPresent =
        std::chrono::steady_clock::time_point::min();

    if (!ui.debugLines.empty() && !videoFrameResult.debugLine.empty()) {
      ui.debugLines.push_back(videoFrameResult.debugLine);
    }
    const bool seekingStateChanged = seekingNow != lastWindowSeeking;
    bool seekingRefreshDue = false;
    if (seekingNow || lastWindowSeeking) {
      const auto now = std::chrono::steady_clock::now();
      seekingRefreshDue =
          lastSeekingPresent == std::chrono::steady_clock::time_point::min() ||
          (now - lastSeekingPresent) >= kSeekingRefreshInterval;
    }
    const bool needsPresent = frameChanged || forcePresentNow ||
                              seekingRefreshDue || seekingStateChanged;

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
      if (seekingNow) {
        lastSeekingPresent = std::chrono::steady_clock::now();
      } else {
        lastSeekingPresent = std::chrono::steady_clock::time_point::min();
      }
      lastWindowSeeking = seekingNow;
    }
  }
}

}  // namespace playback_framebuffer_presenter
