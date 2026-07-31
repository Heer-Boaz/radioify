#include "window_presenter.h"

#include <cstdarg>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>

#include "core/native_wait_handle.h"
#include "core/windows_app_resources.h"
#include "core/windows_handle.h"
#include "presenter.h"
#include "playback/session/window_presentation.h"
#include "runtime_helpers.h"
#include "timing_log.h"

namespace {

void appendWindowPresenterTimingLog(const char* fmt, ...) {
#if RADIOIFY_ENABLE_TIMING_LOG
  if (!fmt || fmt[0] == '\0') return;
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  int written = std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (written <= 0) return;
  if (written >= static_cast<int>(sizeof(buf))) {
    written = static_cast<int>(sizeof(buf)) - 1;
  }
  std::lock_guard<std::mutex> lock(timingLogMutex());
  std::ofstream f(radioifyLogPath(), std::ios::app);
  if (!f) return;
  f << radioifyLogTimestamp() << " "
    << std::string(buf, buf + written) << "\n";
  f.flush();
#else
  (void)fmt;
#endif
}

struct WindowStartGate {
  std::mutex mutex;
  std::condition_variable ready;
  bool completed = false;
  bool opened = false;
};

}  // namespace

struct WindowPresenter::Impl {
  VideoWindow window;
  GpuVideoFrameCache frameCache;
  playback_framebuffer_presenter::FrameSnapshotRequest frameSnapshotRequest;
  std::atomic<WindowThreadState> threadState{WindowThreadState::Disabled};
  std::atomic<bool> forcePresent{false};
  std::atomic<HWND> windowHandle{nullptr};
  UniqueWindowsHandle wakeEvent{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  std::thread thread;

  Impl() { window.SetVsync(true); }

  ~Impl() {
    stop();
  }

  void notify() {
    if (wakeEvent) {
      SetEvent(wakeEvent.get());
    }
  }

  bool start(Player& player, const std::function<WindowUiState()>& buildUiState,
             const std::function<bool()>& uiRefreshActive,
             const playback_framebuffer_presenter::TextGridPresentationProvider&
                 buildTextGridPresentation,
             const PlaybackSessionContinuationState* initialState) {
    if (thread.joinable()) {
      threadState.store(WindowThreadState::Enabled, std::memory_order_relaxed);
      forcePresent.store(true, std::memory_order_relaxed);
      notify();
      return true;
    }

    auto startGate = std::make_shared<WindowStartGate>();
    frameSnapshotRequest.reset();
    windowHandle.store(nullptr, std::memory_order_release);
    threadState.store(WindowThreadState::Enabled, std::memory_order_relaxed);
    forcePresent.store(true, std::memory_order_relaxed);

    thread = std::thread(
        [this, &player, buildUiState, uiRefreshActive, buildTextGridPresentation,
         startGate, initialState]() {
          const bool opened =
              window.Open(VideoWindow::kDefaultVideoClientWidth,
                          VideoWindow::kDefaultVideoClientHeight,
                          RADIOIFY_APP_NAME " Output", false);
          if (opened) {
            windowHandle.store(window.NativeWindowHandle(),
                               std::memory_order_release);
            window.EnableFileDrop();
            if (initialState && initialState->hasLayout) {
              playback_session_window::applyPlacement(
                  window, initialState->windowPlacement);
            }
          }
          {
            std::lock_guard<std::mutex> lock(startGate->mutex);
            startGate->opened = opened;
            startGate->completed = true;
          }
          startGate->ready.notify_one();

          if (opened) {
            playback_framebuffer_presenter::runFramebufferPresenterLoop(
                player, window, frameCache, threadState, forcePresent,
                NativeWaitHandle(wakeEvent.get()), frameSnapshotRequest,
                uiRefreshActive, buildUiState, buildTextGridPresentation);
            window.Close();
            windowHandle.store(nullptr, std::memory_order_release);
          }

          threadState.store(WindowThreadState::Disabled,
                            std::memory_order_relaxed);
          forcePresent.store(false, std::memory_order_relaxed);
        });

    bool opened = false;
    {
      std::unique_lock<std::mutex> lock(startGate->mutex);
      startGate->ready.wait(lock, [&]() { return startGate->completed; });
      opened = startGate->opened;
    }

    if (!opened) {
      threadState.store(WindowThreadState::Disabled, std::memory_order_relaxed);
      forcePresent.store(false, std::memory_order_relaxed);
      notify();
      if (thread.joinable()) {
        thread.join();
      }
      return false;
    }

    notify();
    return true;
  }

  void stop() {
    HWND hwnd = nativeWindowHandle();
    appendWindowPresenterTimingLog(
        "window_presenter_stop begin joinable=%d open=%d visible=%d",
        thread.joinable() ? 1 : 0, hwnd && IsWindow(hwnd) ? 1 : 0,
        hwnd && IsWindowVisible(hwnd) ? 1 : 0);

    if (thread.joinable()) {
      appendWindowPresenterTimingLog("window_presenter_stop join_begin");
      threadState.store(WindowThreadState::Stopping, std::memory_order_relaxed);
      forcePresent.store(false, std::memory_order_relaxed);
      frameSnapshotRequest.cancel(
          "Frame capture was cancelled because the video window stopped.");
      notify();
      thread.join();
      appendWindowPresenterTimingLog("window_presenter_stop join_end");
    }

    threadState.store(WindowThreadState::Disabled, std::memory_order_relaxed);
    forcePresent.store(false, std::memory_order_relaxed);
    windowHandle.store(nullptr, std::memory_order_release);
    {
      std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
      frameCache.Reset();
    }
    appendWindowPresenterTimingLog("window_presenter_stop end");
  }

  void requestPresent() {
    forcePresent.store(true, std::memory_order_relaxed);
    notify();
  }

  VideoFrameSnapshotResult captureCurrentFrame() {
    VideoFrameSnapshotResult unavailable;
    if (!thread.joinable() ||
        threadState.load(std::memory_order_relaxed) !=
            WindowThreadState::Enabled) {
      unavailable.error =
          "Frame capture is available while the video window is visible.";
      return unavailable;
    }
    if (!frameSnapshotRequest.begin()) {
      unavailable.error = "Another frame capture is already in progress.";
      return unavailable;
    }
    forcePresent.store(true, std::memory_order_relaxed);
    notify();
    return frameSnapshotRequest.wait();
  }

  HWND nativeWindowHandle() const {
    return windowHandle.load(std::memory_order_acquire);
  }
};

WindowPresenter::WindowPresenter()
    : impl_(std::make_unique<Impl>()) {}

WindowPresenter::~WindowPresenter() = default;

bool WindowPresenter::start(
    Player& player, const std::function<WindowUiState()>& buildUiState,
    const std::function<bool()>& uiRefreshActive,
    const playback_framebuffer_presenter::TextGridPresentationProvider&
        buildTextGridPresentation,
    const PlaybackSessionContinuationState* initialState) {
  return impl_->start(player, buildUiState, uiRefreshActive,
                      buildTextGridPresentation, initialState);
}

void WindowPresenter::stop() { impl_->stop(); }

void WindowPresenter::requestPresent() { impl_->requestPresent(); }

VideoFrameSnapshotResult WindowPresenter::captureCurrentFrame() {
  return impl_->captureCurrentFrame();
}

bool WindowPresenter::isOpen() const {
  HWND hwnd = impl_->nativeWindowHandle();
  return hwnd && IsWindow(hwnd);
}

bool WindowPresenter::isVisible() const {
  HWND hwnd = impl_->nativeWindowHandle();
  return hwnd && IsWindowVisible(hwnd);
}

HWND WindowPresenter::nativeWindowHandle() const {
  return impl_->nativeWindowHandle();
}

bool WindowPresenter::consumeCloseRequested() {
  return impl_->window.ConsumeCloseRequested();
}

NativeWaitHandle WindowPresenter::closeRequestedWaitHandle() const {
  return impl_->window.CloseRequestedWaitHandle();
}

VideoWindow& WindowPresenter::window() { return impl_->window; }

const VideoWindow& WindowPresenter::window() const {
  return impl_->window;
}
