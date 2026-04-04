#include "playback_framebuffer_presenter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <utility>

#include "audioplayback.h"

namespace playback_framebuffer_presenter {
namespace {

constexpr auto kOverlayRefreshInterval = std::chrono::milliseconds(100);

void waitForPresenterWake(HANDLE wakeEvent) {
  if (wakeEvent) {
    WaitForSingleObject(wakeEvent, INFINITE);
    return;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
    if (timeoutMs < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      return;
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(std::max(0, timeoutMs)));
    return;
  }
  const DWORD waitMs =
      timeoutMs < 0 ? INFINITE : static_cast<DWORD>(std::max(0, timeoutMs));
  WaitForMultipleObjects(handleCount, handles, FALSE, waitMs);
}

}  // namespace

WindowUiState buildPlaybackFramebufferUiState(
    const std::string& windowTitle, VideoWindow& videoWindow, Player& player,
    SubtitleManager& subtitleManager, PlaybackSessionState playbackState,
    bool audioOk, bool hasSubtitles,
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
  overlayInputs.audioSupports50HzToggle = audioOk && audioSupports50HzToggle();
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
    const std::function<WindowUiState()>& buildUiState) {
  if (!overlayVisible || !buildUiState) {
    return;
  }

  VideoFrame localFrame;
  uint64_t lastCounter = player.videoFrameCounter();
  auto lastOverlayPresent = std::chrono::steady_clock::time_point::min();
  const HANDLE frameEvent = player.videoFrameWaitHandle();
  while (threadState.load(std::memory_order_relaxed) !=
         WindowThreadState::Stopping) {
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
    if (!forcePresentRequested) {
      int waitTimeoutMs = -1;
      if (overlayVisibleRequested || seekingRequested) {
        const auto now = std::chrono::steady_clock::now();
        if (lastOverlayPresent == std::chrono::steady_clock::time_point::min()) {
          waitTimeoutMs = 0;
        } else {
          waitTimeoutMs = std::max(
              0, static_cast<int>(std::chrono::duration_cast<
                                       std::chrono::milliseconds>(
                                       (lastOverlayPresent +
                                        kOverlayRefreshInterval) -
                                       now)
                                       .count()));
        }
      }
      waitForPresenterActivity(wakeEvent, frameEvent, waitTimeoutMs);
    }

    if (threadState.load(std::memory_order_relaxed) ==
        WindowThreadState::Stopping) {
      break;
    }

    uint64_t counterNow = player.videoFrameCounter();
    bool frameChanged = false;
    if (counterNow != lastCounter) {
      frameChanged = player.tryGetVideoFrame(&localFrame);
      lastCounter = counterNow;
    }
    if (threadState.load(std::memory_order_relaxed) ==
        WindowThreadState::Stopping) {
      break;
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

    WindowUiState ui = buildUiState();
    bool overlayVisibleNow = ui.overlayAlpha > 0.01f;
    bool seekingNow = player.isSeeking();
    bool forcePresentNow = forcePresent.exchange(false, std::memory_order_relaxed);
    bool overlayRefreshDue = false;
    if (overlayVisibleNow || seekingNow) {
      const auto now = std::chrono::steady_clock::now();
      overlayRefreshDue =
          lastOverlayPresent == std::chrono::steady_clock::time_point::min() ||
          (now - lastOverlayPresent) >= kOverlayRefreshInterval;
    }
    bool needsPresent = frameChanged || forcePresentNow || overlayRefreshDue;

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
    }
  }
}

}  // namespace playback_framebuffer_presenter
