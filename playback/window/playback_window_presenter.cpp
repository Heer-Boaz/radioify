#include "playback_window_presenter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "audioplayback.h"

namespace playback_window_presenter {

WindowUiState buildPlaybackWindowUiState(
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

void runWindowPresenterLoop(
    Player& player, VideoWindow& videoWindow, GpuVideoFrameCache& frameCache,
    std::atomic<WindowThreadState>& threadState,
    std::atomic<bool>& forcePresent, std::mutex& presentMutex,
    std::condition_variable& presentCv,
    const std::function<bool()>& overlayVisible,
    const std::function<WindowUiState()>& buildUiState) {
  if (!overlayVisible || !buildUiState) {
    return;
  }

  VideoFrame localFrame;
  uint64_t lastCounter = player.videoFrameCounter();
  while (threadState.load(std::memory_order_relaxed) !=
         WindowThreadState::Stopping) {
    if (threadState.load(std::memory_order_relaxed) ==
        WindowThreadState::Disabled) {
      std::unique_lock<std::mutex> lock(presentMutex);
      presentCv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
        return threadState.load(std::memory_order_relaxed) ==
                   WindowThreadState::Stopping ||
               threadState.load(std::memory_order_relaxed) ==
                   WindowThreadState::Enabled;
      });
      continue;
    }

    if (!videoWindow.IsOpen() || !videoWindow.IsVisible()) {
      std::unique_lock<std::mutex> lock(presentMutex);
      presentCv.wait_for(lock, std::chrono::milliseconds(50));
      continue;
    }

    bool waitedForFrame = false;
    bool shouldWaitForFrame = !forcePresent.load(std::memory_order_relaxed) &&
                              !overlayVisible() && !player.isSeeking();
    if (shouldWaitForFrame) {
      waitedForFrame = true;
      player.waitForVideoFrame(lastCounter, 16);
      if (threadState.load(std::memory_order_relaxed) ==
          WindowThreadState::Stopping) {
        break;
      }
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
    bool forcePresentNow = forcePresent.exchange(false, std::memory_order_relaxed);
    bool needsPresent = frameChanged || forcePresentNow || overlayVisibleNow ||
                        player.isSeeking();

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
    } else if (!waitedForFrame) {
      std::unique_lock<std::mutex> lock(presentMutex);
      presentCv.wait_for(lock, std::chrono::milliseconds(10));
    }
  }
}

}  // namespace playback_window_presenter
