#include "window_presenter.h"

#include <cstdarg>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>

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
  std::atomic<WindowThreadState> threadState{WindowThreadState::Disabled};
  std::atomic<bool> forcePresent{false};
  HANDLE wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  std::thread thread;

  Impl() { window.SetVsync(true); }

  ~Impl() {
    stop();
    if (wakeEvent) {
      CloseHandle(wakeEvent);
      wakeEvent = nullptr;
    }
  }

  void notify() {
    if (wakeEvent) {
      SetEvent(wakeEvent);
    }
  }

  bool start(Player& player, const std::function<WindowUiState()>& buildUiState,
             const std::function<bool()>& overlayVisible,
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
    threadState.store(WindowThreadState::Enabled, std::memory_order_relaxed);
    forcePresent.store(true, std::memory_order_relaxed);

    thread = std::thread(
        [this, &player, buildUiState, overlayVisible, buildTextGridPresentation,
         startGate, initialState]() {
          const bool startFullscreen =
              !initialState || !initialState->hasLayout ||
              playback_session_window::shouldStartFullscreen(
                  initialState->windowPlacement);
          const bool opened =
              window.Open(1280, 720, "Radioify Output", startFullscreen);
          if (opened) {
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
                wakeEvent, overlayVisible, buildUiState,
                buildTextGridPresentation);
            window.Close();
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
    appendWindowPresenterTimingLog(
        "window_presenter_stop begin joinable=%d open=%d visible=%d",
        thread.joinable() ? 1 : 0, window.IsOpen() ? 1 : 0,
        window.IsVisible() ? 1 : 0);

    if (thread.joinable()) {
      appendWindowPresenterTimingLog("window_presenter_stop join_begin");
      threadState.store(WindowThreadState::Stopping, std::memory_order_relaxed);
      forcePresent.store(false, std::memory_order_relaxed);
      notify();
      thread.join();
      appendWindowPresenterTimingLog("window_presenter_stop join_end");
    }

    threadState.store(WindowThreadState::Disabled, std::memory_order_relaxed);
    forcePresent.store(false, std::memory_order_relaxed);
    frameCache.Reset();
    appendWindowPresenterTimingLog("window_presenter_stop end");
  }

  void requestPresent() {
    forcePresent.store(true, std::memory_order_relaxed);
    notify();
  }
};

WindowPresenter::WindowPresenter()
    : impl_(std::make_unique<Impl>()) {}

WindowPresenter::~WindowPresenter() = default;

bool WindowPresenter::start(
    Player& player, const std::function<WindowUiState()>& buildUiState,
    const std::function<bool()>& overlayVisible,
    const playback_framebuffer_presenter::TextGridPresentationProvider&
        buildTextGridPresentation,
    const PlaybackSessionContinuationState* initialState) {
  return impl_->start(player, buildUiState, overlayVisible,
                      buildTextGridPresentation, initialState);
}

void WindowPresenter::stop() { impl_->stop(); }

void WindowPresenter::requestPresent() { impl_->requestPresent(); }

bool WindowPresenter::isOpen() const { return impl_->window.IsOpen(); }

bool WindowPresenter::isVisible() const {
  return impl_->window.IsVisible();
}

bool WindowPresenter::consumeCloseRequested() {
  return impl_->window.ConsumeCloseRequested();
}

HANDLE WindowPresenter::closeRequestedWaitHandle() const {
  return impl_->window.CloseRequestedWaitHandle();
}

VideoWindow& WindowPresenter::window() { return impl_->window; }

const VideoWindow& WindowPresenter::window() const {
  return impl_->window;
}

GpuVideoFrameCache& WindowPresenter::frameCache() {
  return impl_->frameCache;
}
