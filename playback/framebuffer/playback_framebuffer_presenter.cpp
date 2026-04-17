#include "playback_framebuffer_presenter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>

#include "audioplayback.h"
#include "core/windows_message_pump.h"
#include "gpu_shared.h"
#include "playback_mini_player_tui.h"

namespace playback_framebuffer_presenter {
namespace {

constexpr auto kOverlayRefreshInterval = std::chrono::milliseconds(100);
constexpr auto kPictureInPictureTextRefreshInterval =
    std::chrono::milliseconds(250);

void waitForPresenterWake(HANDLE wakeEvent) {
  if (wakeEvent) {
    waitForHandlesAndPumpThreadWindowMessages(1, &wakeEvent, INFINITE);
    return;
  }
  waitForHandlesAndPumpThreadWindowMessages(0, nullptr, 50);
}

void waitForPresenterActivity(HANDLE wakeEvent, HANDLE frameEvent,
                              int timeoutMs) {
  HANDLE handles[2];
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

WindowUiState buildPlaybackFramebufferUiState(
    const std::string& windowTitle, VideoWindow& videoWindow, Player& player,
    SubtitleManager& subtitleManager, PlaybackSessionState playbackState,
    bool audioOk, bool canPlayPrevious, bool canPlayNext, bool hasSubtitles,
    std::atomic<bool>& enableSubtitlesShared,
    std::atomic<bool>& windowLocalSeekRequested,
    std::atomic<double>& windowPendingSeekTargetSec,
    std::atomic<int>& overlayControlHover, bool overlayVisibleNow) {
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
  bool seekingOverlay = player.isSeeking() ||
                        windowLocalSeekRequested.load(std::memory_order_relaxed);
  if (seekingOverlay && totalSec > 0.0 && std::isfinite(totalSec)) {
    displaySec = std::clamp(
        windowPendingSeekTargetSec.load(std::memory_order_relaxed), 0.0,
        totalSec);
  }
  const bool subtitlesEnabledNow =
      enableSubtitlesShared.load(std::memory_order_relaxed);
  bool pausedNow = playbackState == PlaybackSessionState::Paused ||
                   player.state() == PlayerState::Paused;
  bool audioFinishedNow = audioOk && audioIsFinished();

  playback_overlay::PlaybackOverlayInputs overlayInputs;
  overlayInputs.windowTitle = windowTitle;
  overlayInputs.audioOk = audioOk;
  overlayInputs.playPauseAvailable =
      playbackState == PlaybackSessionState::Active ||
      playbackState == PlaybackSessionState::Paused;
  overlayInputs.audioSupports50HzToggle = audioOk && audioSupports50HzToggle();
  overlayInputs.canPlayPrevious = canPlayPrevious;
  overlayInputs.canPlayNext = canPlayNext;
  overlayInputs.radioEnabled = audioIsRadioEnabled();
  overlayInputs.hz50Enabled = audioIs50HzEnabled();
  overlayInputs.canCycleAudioTracks = audioOk && player.canCycleAudioTracks();
  overlayInputs.activeAudioTrackLabel =
      audioOk ? player.activeAudioTrackLabel() : "N/A";
  overlayInputs.subtitleManager = &subtitleManager;
  overlayInputs.hasSubtitles = hasSubtitles;
  overlayInputs.subtitlesEnabled = subtitlesEnabledNow;
  overlayInputs.subtitleClockUs = clockUs;
  overlayInputs.seekingOverlay =
      player.isSeeking() || windowLocalSeekRequested.load(std::memory_order_relaxed);
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
  return playback_overlay::buildWindowUiState(
      overlayState, overlayControlHover.load(std::memory_order_relaxed));
}

void runFramebufferPresenterLoop(
    Player& player, VideoWindow& videoWindow, GpuVideoFrameCache& frameCache,
    std::atomic<WindowThreadState>& threadState,
    std::atomic<bool>& forcePresent, HANDLE wakeEvent,
    const std::function<bool()>& overlayVisible,
    const std::function<WindowUiState()>& buildUiState,
    const PictureInPictureTextGridProvider& buildPictureInPictureTextGrid) {
  if (!overlayVisible || !buildUiState) {
    return;
  }

  VideoFrame localFrame;
  std::vector<ScreenCell> pictureInPictureTextCells;
  GpuTextGridFrame pictureInPictureTextFrame;
  uint64_t lastCounter = player.videoFrameCounter();
  auto lastOverlayPresent = std::chrono::steady_clock::time_point::min();
  bool lastWindowOverlayVisible = false;
  bool lastWindowSeeking = false;
  auto lastPictureInPictureTextPresent =
      std::chrono::steady_clock::time_point::min();
  int lastPictureInPictureTextWidth = 0;
  int lastPictureInPictureTextHeight = 0;
  int lastPictureInPictureTextCellWidth = 0;
  int lastPictureInPictureTextCellHeight = 0;
  const HANDLE frameEvent = player.videoFrameWaitHandle();
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
      waitForPresenterWake(wakeEvent);
      continue;
    }

    const bool forcePresentRequested =
        forcePresent.load(std::memory_order_relaxed);
    const bool overlayVisibleRequested = overlayVisible();
    const bool seekingRequested = player.isSeeking();
    const bool pictureInPictureTextRequested =
        videoWindow.IsPictureInPicture() &&
        videoWindow.IsPictureInPictureTextMode();
    if (!forcePresentRequested) {
      int waitTimeoutMs = -1;
      auto tightenWaitTimeout = [&](int candidateMs) {
        if (candidateMs < 0) return;
        waitTimeoutMs = waitTimeoutMs < 0
                            ? candidateMs
                            : std::min(waitTimeoutMs, candidateMs);
      };
      if (overlayVisibleRequested || seekingRequested) {
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
      if (pictureInPictureTextRequested) {
        const auto now = std::chrono::steady_clock::now();
        if (lastPictureInPictureTextPresent ==
            std::chrono::steady_clock::time_point::min()) {
          tightenWaitTimeout(0);
        } else {
          tightenWaitTimeout(std::max(
              0, static_cast<int>(std::chrono::duration_cast<
                                       std::chrono::milliseconds>(
                                       (lastPictureInPictureTextPresent +
                                        kPictureInPictureTextRefreshInterval) -
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

    uint64_t counterNow = player.videoFrameCounter();
    bool frameChanged = false;
    bool textFrameChanged = false;
    if (counterNow != lastCounter) {
      textFrameChanged = player.tryGetVideoFrame(&localFrame);
      frameChanged = textFrameChanged;
      lastCounter = counterNow;
    }
    if (threadState.load(std::memory_order_relaxed) ==
        WindowThreadState::Stopping) {
      break;
    }
    const bool textGridPresentationActive =
        videoWindow.IsPictureInPicture() &&
        videoWindow.IsPictureInPictureTextMode();
    if (textGridPresentationActive && localFrame.width <= 0 &&
        player.copyCurrentVideoFrame(&localFrame)) {
      textFrameChanged = true;
    }
    if (frameChanged && textGridPresentationActive) {
      frameChanged = false;
    }
    if (frameChanged) {
      if (localFrame.format != VideoPixelFormat::HWTexture ||
          !localFrame.hwTexture) {
        frameChanged = false;
      } else {
        D3D11_TEXTURE2D_DESC desc{};
        localFrame.hwTexture->GetDesc(&desc);
        bool is10Bit = (desc.Format == DXGI_FORMAT_P010);

        ID3D11Device* device = getSharedGpuDevice();
        if (device) {
          Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
          device->GetImmediateContext(&context);
          if (context) {
            std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
            bool updated = frameCache.Update(
                device, context.Get(), localFrame.hwTexture.Get(),
                localFrame.hwTextureArrayIndex, localFrame.width,
                localFrame.height, localFrame.fullRange, localFrame.yuvMatrix,
                localFrame.yuvTransfer, is10Bit ? 10 : 8,
                localFrame.rotationQuarterTurns);
            if (!updated) {
              frameChanged = false;
            }
          }
        }
      }
    }

    bool seekingNow = player.isSeeking();
    bool forcePresentNow =
        forcePresent.exchange(false, std::memory_order_relaxed);
    if (videoWindow.IsPictureInPicture() &&
        videoWindow.IsPictureInPictureTextMode()) {
      const int windowWidth = videoWindow.GetWidth();
      const int windowHeight = videoWindow.GetHeight();
      int cellWidth = 1;
      int cellHeight = 1;
      videoWindow.GetPictureInPictureTextCellSize(cellWidth, cellHeight);
      const auto now = std::chrono::steady_clock::now();
      const bool refreshDue =
          lastPictureInPictureTextPresent ==
              std::chrono::steady_clock::time_point::min() ||
          (now - lastPictureInPictureTextPresent) >=
              kPictureInPictureTextRefreshInterval;
      const bool sizeChanged = windowWidth != lastPictureInPictureTextWidth ||
                               windowHeight != lastPictureInPictureTextHeight ||
                               cellWidth !=
                                   lastPictureInPictureTextCellWidth ||
                               cellHeight !=
                                   lastPictureInPictureTextCellHeight;

      if (textFrameChanged || forcePresentNow || refreshDue || sizeChanged) {
        int textCols = 0;
        int textRows = 0;
        const VideoFrame* textFrame =
            (localFrame.width > 0 && localFrame.height > 0) ? &localFrame
                                                            : nullptr;
        if (buildPictureInPictureTextGrid &&
            buildPictureInPictureTextGrid(windowWidth, windowHeight, cellWidth,
                                          cellHeight, textFrame,
                                          textFrameChanged,
                                          pictureInPictureTextCells, textCols,
                                          textRows)) {
          buildGpuTextGridFrameFromScreenCells(
              pictureInPictureTextCells, textCols, textRows,
              pictureInPictureTextFrame);
          videoWindow.PresentGpuTextGrid(pictureInPictureTextFrame, true);
        }
        lastPictureInPictureTextPresent = std::chrono::steady_clock::now();
        lastPictureInPictureTextWidth = windowWidth;
        lastPictureInPictureTextHeight = windowHeight;
        lastPictureInPictureTextCellWidth = cellWidth;
        lastPictureInPictureTextCellHeight = cellHeight;
      }
      continue;
    }
    lastPictureInPictureTextPresent =
        std::chrono::steady_clock::time_point::min();

    WindowUiState ui = buildUiState();
    bool overlayVisibleNow = ui.overlayAlpha > 0.01f;
    bool overlayStateChanged = overlayVisibleNow != lastWindowOverlayVisible ||
                               seekingNow != lastWindowSeeking;
    bool overlayRefreshDue = false;
    if (overlayVisibleNow || seekingNow) {
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
      frameCache.WaitForFrameLatency(
          16, videoWindow.GetFrameLatencyWaitableObject());
      if (threadState.load(std::memory_order_relaxed) ==
          WindowThreadState::Stopping) {
        break;
      }
      if (frameChanged) {
        videoWindow.Present(frameCache, ui, true);
      } else {
        videoWindow.PresentOverlay(frameCache, ui, true);
      }
      if (overlayVisibleNow || seekingNow) {
        lastOverlayPresent = std::chrono::steady_clock::now();
      }
      lastWindowOverlayVisible = overlayVisibleNow;
      lastWindowSeeking = seekingNow;
    }
  }
}

}  // namespace playback_framebuffer_presenter
