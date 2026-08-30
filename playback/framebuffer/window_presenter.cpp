#include "window_presenter.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "core/native_wait_handle.h"
#include "core/pumpable_worker_thread.h"
#include "core/thread_dispatch_queue.h"
#include "core/waitable_signal.h"
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

std::string nativePlaybackWindowTitle(const std::string& mediaTitle) {
  return mediaTitle.empty() ? std::string(RADIOIFY_APP_NAME)
                            : mediaTitle + " - " RADIOIFY_APP_NAME;
}

}  // namespace

struct WindowPresenter::Impl {
  Player& player;
  GpuRuntime& gpu;
  const std::string nativeWindowTitle;
  const std::shared_ptr<playback_framebuffer_presenter::PresentationSource>
      presentationSource;
  VideoWindow window;
  GpuVideoFrameCache frameCache;
  ThreadDispatchQueue dispatch;
  std::atomic<WindowThreadState> threadState{WindowThreadState::Disabled};
  std::atomic<bool> forcePresent{false};
  std::atomic<bool> cursorVisible{true};
  std::atomic<HWND> windowHandle{nullptr};
  UniqueWindowsHandle wakeEvent{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  WaitableSignal lifecycleChanged;
  std::atomic<WindowPresenter::Lifecycle> lifecycle{
      WindowPresenter::Lifecycle::Closed};
  PumpableWorkerThread thread;
  Impl(Player& player, GpuRuntime& gpu, std::string mediaTitle,
       std::shared_ptr<playback_framebuffer_presenter::PresentationSource>
           presentationSource,
       SystemMediaCommandOwner systemMediaCommandOwner)
      : player(player),
        gpu(gpu),
        nativeWindowTitle(nativePlaybackWindowTitle(mediaTitle)),
        presentationSource(std::move(presentationSource)),
        window(gpu, systemMediaCommandOwner) {
    if (!this->presentationSource) {
      throw std::invalid_argument(
          "WindowPresenter requires a presentation source");
    }
    window.SetVsync(true);
  }

  ~Impl() {
    stop();
  }

  void notify() {
    if (wakeEvent) {
      SetEvent(wakeEvent.get());
    }
  }

  bool requestStart() {
    const WindowPresenter::Lifecycle current =
        lifecycle.load(std::memory_order_acquire);
    if (current == WindowPresenter::Lifecycle::Open ||
        current == WindowPresenter::Lifecycle::Opening) {
      return true;
    }
    if (current != WindowPresenter::Lifecycle::Closed || thread.joinable()) {
      return false;
    }

    lifecycleChanged.clear();
    windowHandle.store(nullptr, std::memory_order_release);
    cursorVisible.store(true, std::memory_order_relaxed);
    threadState.store(WindowThreadState::Enabled, std::memory_order_relaxed);
    forcePresent.store(true, std::memory_order_relaxed);
    lifecycle.store(WindowPresenter::Lifecycle::Opening,
                    std::memory_order_release);
    if (!thread.start([this]() {
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
      WindowPresenter::Lifecycle expected =
          WindowPresenter::Lifecycle::Opening;
      if (opened) {
        (void)lifecycle.compare_exchange_strong(
            expected, WindowPresenter::Lifecycle::Open,
            std::memory_order_release, std::memory_order_acquire);
      } else {
        (void)lifecycle.compare_exchange_strong(
            expected, WindowPresenter::Lifecycle::Failed,
            std::memory_order_release, std::memory_order_acquire);
      }
      lifecycleChanged.signal();

      if (opened) {
        playback_framebuffer_presenter::runFramebufferPresenterLoop(
            player, gpu, window, frameCache, threadState, forcePresent,
            NativeWaitHandle(wakeEvent.get()), dispatch, *presentationSource);
        dispatch.close();
        window.Close();
        windowHandle.store(nullptr, std::memory_order_release);
      } else {
        dispatch.close();
      }

      threadState.store(WindowThreadState::Disabled,
                        std::memory_order_release);
      forcePresent.store(false, std::memory_order_relaxed);
      if (lifecycle.load(std::memory_order_acquire) ==
          WindowPresenter::Lifecycle::Open) {
        lifecycle.store(WindowPresenter::Lifecycle::Failed,
                        std::memory_order_release);
      }
      lifecycleChanged.signal();
    })) {
      threadState.store(WindowThreadState::Disabled,
                        std::memory_order_release);
      forcePresent.store(false, std::memory_order_relaxed);
      lifecycle.store(WindowPresenter::Lifecycle::Failed,
                      std::memory_order_release);
      lifecycleChanged.signal();
      return false;
    }
    notify();
    return true;
  }

  void requestStop() {
    HWND hwnd = nativeWindowHandle();
    appendWindowPresenterTimingLog(
        "window_presenter_stop begin joinable=%d open=%d visible=%d",
        thread.joinable() ? 1 : 0, hwnd && IsWindow(hwnd) ? 1 : 0,
        hwnd && IsWindowVisible(hwnd) ? 1 : 0);

    if (thread.joinable()) {
      lifecycle.store(WindowPresenter::Lifecycle::Closing,
                      std::memory_order_release);
      threadState.store(WindowThreadState::Stopping, std::memory_order_relaxed);
      forcePresent.store(false, std::memory_order_relaxed);
      dispatch.close();
      notify();
    }
  }

  bool stopReady() const {
    return thread.ready();
  }

  bool finishStop() {
    if (thread.joinable()) {
      if (!thread.ready()) return false;
      appendWindowPresenterTimingLog("window_presenter_stop join_begin");
      (void)thread.finish();
      appendWindowPresenterTimingLog("window_presenter_stop join_end");
    }

    threadState.store(WindowThreadState::Disabled, std::memory_order_relaxed);
    forcePresent.store(false, std::memory_order_relaxed);
    windowHandle.store(nullptr, std::memory_order_release);
    cursorVisible.store(true, std::memory_order_relaxed);
    lifecycle.store(WindowPresenter::Lifecycle::Closed,
                    std::memory_order_release);
    lifecycleChanged.clear();
    {
      std::lock_guard<std::recursive_mutex> lock(gpu.mutex());
      frameCache.Reset();
    }
    appendWindowPresenterTimingLog("window_presenter_stop end");
    return true;
  }

  void stop() {
    requestStop();
    if (thread.joinable()) thread.join();
    (void)finishStop();
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
      if (!window.IsOpen() || !window.IsVisible()) {
        result.error =
            "Frame capture is available while the video window is visible.";
        return;
      }
      result = window.CaptureCurrentFrame(
          frameCache, presentationSource->windowUiState());
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

WindowPresenter::WindowPresenter(
    Player& player, GpuRuntime& gpu, std::string mediaTitle,
    std::shared_ptr<playback_framebuffer_presenter::PresentationSource>
        presentationSource,
    SystemMediaCommandOwner systemMediaCommandOwner)
    : impl_(std::make_unique<Impl>(
          player, gpu, std::move(mediaTitle),
          std::move(presentationSource), systemMediaCommandOwner)) {}

WindowPresenter::~WindowPresenter() = default;

bool WindowPresenter::requestStart() { return impl_->requestStart(); }

WindowPresenter::Lifecycle WindowPresenter::lifecycle() const {
  return impl_->lifecycle.load(std::memory_order_acquire);
}

bool WindowPresenter::consumeLifecycleChange() {
  return impl_->lifecycleChanged.consume();
}

NativeWaitHandle WindowPresenter::lifecycleWaitHandle() const {
  return impl_->lifecycleChanged.nativeWaitHandle();
}

void WindowPresenter::requestStop() { impl_->requestStop(); }

bool WindowPresenter::stopReady() const { return impl_->stopReady(); }

bool WindowPresenter::finishStop() { return impl_->finishStop(); }

NativeWaitHandle WindowPresenter::stopWaitHandle() const {
  return impl_->thread.waitHandle();
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
