#include "window_presenter.h"

#include <cstdarg>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>

#include "core/native_wait_handle.h"
#include "core/thread_dispatch_queue.h"
#include "core/windows_app_resources.h"
#include "core/windows_handle.h"
#include "playback/framebuffer/window_presentation.h"
#include "presenter.h"
#include "runtime_helpers.h"
#include "timing_log.h"

namespace {

using playback_framebuffer_presenter::WindowThreadState;

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
  ThreadDispatchQueue dispatch;
  std::atomic<WindowThreadState> threadState{WindowThreadState::Disabled};
  std::atomic<bool> forcePresent{false};
  std::atomic<bool> cursorVisible{true};
  std::atomic<HWND> windowHandle{nullptr};
  UniqueWindowsHandle wakeEvent{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  std::thread thread;
  std::function<WindowUiState()> uiStateBuilder;

  Impl() { window.SetVsync(true); }

  ~Impl() {
    stop();
  }

  void notify() {
    if (wakeEvent) {
      SetEvent(wakeEvent.get());
    }
  }

  bool start(Player& player, const std::string& mediaTitle,
             const std::function<WindowUiState()>& buildUiState,
             const playback_framebuffer_presenter::TextGridPresentationProvider&
                 buildTextGridPresentation) {
    if (thread.joinable()) {
      HWND hwnd = nativeWindowHandle();
      if (hwnd && IsWindow(hwnd)) {
        threadState.store(WindowThreadState::Enabled,
                          std::memory_order_relaxed);
        forcePresent.store(true, std::memory_order_relaxed);
        notify();
        return true;
      }
      stop();
    }

    auto startGate = std::make_shared<WindowStartGate>();
    uiStateBuilder = buildUiState;
    windowHandle.store(nullptr, std::memory_order_release);
    cursorVisible.store(true, std::memory_order_relaxed);
    threadState.store(WindowThreadState::Enabled, std::memory_order_relaxed);
    forcePresent.store(true, std::memory_order_relaxed);
    const std::string nativeWindowTitle =
        mediaTitle.empty() ? std::string(RADIOIFY_APP_NAME)
                           : mediaTitle + " - " RADIOIFY_APP_NAME;

    thread = std::thread(
        [this, &player, buildTextGridPresentation, startGate,
         nativeWindowTitle]() {
          dispatch.openOnCurrentThread();
          const bool opened = window.Open(
              VideoWindow::kDefaultVideoClientWidth,
              VideoWindow::kDefaultVideoClientHeight,
              nativeWindowTitle);
          if (opened) {
            windowHandle.store(window.NativeWindowHandle(),
                               std::memory_order_release);
            window.EnableFileDrop();
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
                NativeWaitHandle(wakeEvent.get()), dispatch, uiStateBuilder,
                buildTextGridPresentation);
            dispatch.close();
            window.Close();
            windowHandle.store(nullptr, std::memory_order_release);
          } else {
            dispatch.close();
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
      uiStateBuilder = {};
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
      dispatch.close();
      notify();
      thread.join();
      appendWindowPresenterTimingLog("window_presenter_stop join_end");
    }

    threadState.store(WindowThreadState::Disabled, std::memory_order_relaxed);
    forcePresent.store(false, std::memory_order_relaxed);
    windowHandle.store(nullptr, std::memory_order_release);
    uiStateBuilder = {};
    cursorVisible.store(true, std::memory_order_relaxed);
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

  bool applyPresentation(WindowPresentationRequest request) {
    bool applied = false;
    const bool executed = dispatch.invoke([this, request, &applied]() {
      applied = window.IsOpen() &&
                playback_window_presentation::apply(window, request);
    });
    if (executed && applied) {
      requestPresent();
    }
    return executed && applied;
  }

  bool restorePresentation(WindowPresentationRequest request,
                           const WindowPlacementState& placement) {
    bool applied = false;
    const bool executed = dispatch.invoke(
        [this, request, placement, &applied]() {
          if (!window.IsOpen()) {
            return;
          }
          applied = playback_window_presentation::restore(
              window, request, placement);
        });
    if (executed && applied) {
      requestPresent();
    }
    return executed && applied;
  }

  bool capturePlacement(WindowPlacementState& placement) {
    bool captured = false;
    const bool executed = dispatch.invoke(
        [this, &placement, &captured]() {
          if (!window.IsOpen()) {
            return;
          }
          playback_window_presentation::capturePlacement(window, placement);
          captured = true;
        });
    return executed && captured;
  }

  bool activate() {
    bool activated = false;
    const bool executed = dispatch.invoke([this, &activated]() {
      if (!window.IsOpen()) {
        return;
      }
      window.Activate();
      activated = true;
    });
    return executed && activated;
  }

  void setCursorVisible(bool visible) {
    if (cursorVisible.exchange(visible, std::memory_order_relaxed) == visible) {
      return;
    }
    (void)dispatch.invoke(
        [this, visible]() { window.SetCursorVisible(visible); });
  }

  VideoFrameSnapshotResult captureCurrentFrame() {
    VideoFrameSnapshotResult unavailable;
    HWND hwnd = nativeWindowHandle();
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
      unavailable.error =
          "Frame capture is available while the video window is visible.";
      return unavailable;
    }
    VideoFrameSnapshotResult result;
    const bool executed = dispatch.invoke([this, &result]() {
      if (!window.IsOpen() || !window.IsVisible() || !uiStateBuilder) {
        result.error =
            "Frame capture is available while the video window is visible.";
        return;
      }
      result = window.CaptureCurrentFrame(frameCache, uiStateBuilder());
    });
    if (!executed) {
      unavailable.error = "The video presenter stopped before frame capture.";
      return unavailable;
    }
    return result;
  }

  HWND nativeWindowHandle() const {
    return windowHandle.load(std::memory_order_acquire);
  }
};

WindowPresenter::WindowPresenter()
    : impl_(std::make_unique<Impl>()) {}

WindowPresenter::~WindowPresenter() = default;

bool WindowPresenter::start(
    Player& player, const std::string& mediaTitle,
    const std::function<WindowUiState()>& buildUiState,
    const playback_framebuffer_presenter::TextGridPresentationProvider&
        buildTextGridPresentation) {
  return impl_->start(player, mediaTitle, buildUiState,
                      buildTextGridPresentation);
}

void WindowPresenter::stop() { impl_->stop(); }

void WindowPresenter::requestPresent() { impl_->requestPresent(); }

bool WindowPresenter::applyPresentation(
    WindowPresentationRequest request) {
  return impl_->applyPresentation(request);
}

bool WindowPresenter::restorePresentation(
    WindowPresentationRequest request,
    const WindowPlacementState& placement) {
  return impl_->restorePresentation(request, placement);
}

bool WindowPresenter::capturePlacement(WindowPlacementState& placement) {
  return impl_->capturePlacement(placement);
}

bool WindowPresenter::activate() { return impl_->activate(); }

void WindowPresenter::setCursorVisible(bool visible) {
  impl_->setCursorVisible(visible);
}

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
