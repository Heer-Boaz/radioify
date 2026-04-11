#include "playback_session_presentation.h"

#include <cstdarg>
#include <atomic>
#include <fstream>
#include <memory>
#include <thread>

#include "player.h"
#include "playback_framebuffer_presenter.h"
#include "playback_session_state.h"
#include "runtime_helpers.h"
#include "timing_log.h"

namespace {

void appendPresentationTimingLog(const char* fmt, ...) {
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

struct PlaybackWindowPresenter {
  VideoWindow window;
  GpuVideoFrameCache frameCache;
  std::atomic<WindowThreadState> threadState{WindowThreadState::Disabled};
  std::atomic<bool> forcePresent{false};
  HANDLE wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  std::thread thread;

  PlaybackWindowPresenter() { window.SetVsync(true); }

  ~PlaybackWindowPresenter() {
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

  bool start(
      Player& player,
      const std::function<WindowUiState()>& buildUiState,
      const std::function<bool()>& overlayVisible) {
    if (thread.joinable()) {
      threadState.store(WindowThreadState::Enabled,
                        std::memory_order_relaxed);
      forcePresent.store(true, std::memory_order_relaxed);
      notify();
      return true;
    }
    if (!window.IsOpen() && !window.Open(1280, 720, "Radioify Output")) {
      threadState.store(WindowThreadState::Disabled,
                        std::memory_order_relaxed);
      forcePresent.store(false, std::memory_order_relaxed);
      return false;
    }
    window.ShowWindow(true);
    threadState.store(WindowThreadState::Enabled, std::memory_order_relaxed);
    forcePresent.store(true, std::memory_order_relaxed);
    thread = std::thread([this, &player, buildUiState, overlayVisible]() {
      playback_framebuffer_presenter::runFramebufferPresenterLoop(
          player, window, frameCache, threadState, forcePresent, wakeEvent,
          overlayVisible, buildUiState);
    });
    notify();
    return true;
  }

  void stop() {
    appendPresentationTimingLog("window_presenter_stop begin joinable=%d open=%d visible=%d",
                                thread.joinable() ? 1 : 0,
                                window.IsOpen() ? 1 : 0,
                                window.IsVisible() ? 1 : 0);
    if (thread.joinable()) {
      appendPresentationTimingLog("window_presenter_stop join_begin");
      threadState.store(WindowThreadState::Stopping, std::memory_order_relaxed);
      forcePresent.store(false, std::memory_order_relaxed);
      notify();
      thread.join();
      appendPresentationTimingLog("window_presenter_stop join_end");
    }
    threadState.store(WindowThreadState::Disabled, std::memory_order_relaxed);
    forcePresent.store(false, std::memory_order_relaxed);
    if (window.IsOpen()) {
      appendPresentationTimingLog("window_presenter_stop close_begin");
      window.SetCursorVisible(true);
      window.ShowWindow(false);
      window.Close();
      appendPresentationTimingLog("window_presenter_stop close_end");
    }
    appendPresentationTimingLog("window_presenter_stop end");
  }

  void requestPresent() {
    forcePresent.store(true, std::memory_order_relaxed);
    notify();
  }
};

void resetWindowOverlayState(std::atomic<int64_t>& overlayUntilMs,
                             std::atomic<int>& overlayControlHover) {
  overlayUntilMs.store(0, std::memory_order_relaxed);
  overlayControlHover.store(-1, std::memory_order_relaxed);
}

}  // namespace

struct PlaybackPresentation::Impl {
  PlaybackWindowPresenter windowPresenter;
  PlaybackLayout desiredLayout = PlaybackLayout::Terminal;
  PlaybackLayout activeLayout = PlaybackLayout::Terminal;

  explicit Impl(PlaybackLayout initialLayout) : desiredLayout(initialLayout) {}

  bool windowRequested() const { return isWindowPlaybackLayout(desiredLayout); }

  bool windowActive() const { return isWindowPlaybackLayout(activeLayout); }
};

PlaybackPresentation::PlaybackPresentation(PlaybackLayout initialLayout)
    : impl_(std::make_unique<Impl>(initialLayout)) {}

PlaybackPresentation::~PlaybackPresentation() = default;

PlaybackPresentation::PlaybackPresentation(PlaybackPresentation&&) noexcept =
    default;

PlaybackPresentation& PlaybackPresentation::operator=(
    PlaybackPresentation&&) noexcept = default;

bool PlaybackPresentation::windowRequested() const {
  return impl_->windowRequested();
}

bool PlaybackPresentation::windowActive() const { return impl_->windowActive(); }

PlaybackRenderMode PlaybackPresentation::renderMode(bool enableAscii) const {
  return resolvePlaybackMode(enableAscii, impl_->activeLayout);
}

void PlaybackPresentation::requestLayout(PlaybackLayout layout) {
  impl_->desiredLayout = layout;
}

PlaybackLayout& PlaybackPresentation::desiredLayout() {
  return impl_->desiredLayout;
}

PlaybackPresenterSyncResult PlaybackPresentation::sync(
    Player& player,
    const std::function<WindowUiState()>& buildUiState,
    const std::function<bool()>& overlayVisible, bool& redraw,
    bool& forceRefreshArt, std::atomic<int64_t>& overlayUntilMs,
    std::atomic<int>& overlayControlHover) {
  PlaybackPresenterSyncResult result;
  result.previousActiveLayout = impl_->activeLayout;
  if (impl_->windowRequested()) {
    if (impl_->windowPresenter.start(player, buildUiState, overlayVisible)) {
      impl_->activeLayout = PlaybackLayout::Window;
    } else {
      impl_->desiredLayout = PlaybackLayout::Terminal;
      impl_->activeLayout = PlaybackLayout::Terminal;
      forceRefreshArt = true;
      redraw = true;
      resetWindowOverlayState(overlayUntilMs, overlayControlHover);
    }
  } else {
    if (impl_->windowActive() || impl_->windowPresenter.window.IsOpen()) {
      resetWindowOverlayState(overlayUntilMs, overlayControlHover);
      impl_->windowPresenter.stop();
    }
    impl_->activeLayout = PlaybackLayout::Terminal;
  }
  result.activeLayout = impl_->activeLayout;
  return result;
}

void PlaybackPresentation::stop() {
  impl_->windowPresenter.stop();
  impl_->activeLayout = PlaybackLayout::Terminal;
}

VideoWindow& PlaybackPresentation::window() { return impl_->windowPresenter.window; }

const VideoWindow& PlaybackPresentation::window() const {
  return impl_->windowPresenter.window;
}

GpuVideoFrameCache& PlaybackPresentation::frameCache() {
  return impl_->windowPresenter.frameCache;
}

void PlaybackPresentation::requestPresent() {
  impl_->windowPresenter.requestPresent();
}
