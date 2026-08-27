#include "playback/video/edit/export.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#include "core/waitable_signal.h"

namespace playback_video_edit {
namespace {

bool validRanges(const std::vector<SourceRange>& ranges) {
  if (ranges.empty()) return false;
  int64_t previousEnd = -1;
  for (const SourceRange& range : ranges) {
    if (range.startUs < 0 || range.endUs <= range.startUs ||
        range.startUs < previousEnd) {
      return false;
    }
    previousEnd = range.endUs;
  }
  return true;
}

}  // namespace

std::filesystem::path uniqueEditedOutputPath(
    const std::filesystem::path& sourcePath) {
  if (sourcePath.empty() || sourcePath.extension().empty()) return {};
  const std::filesystem::path directory = sourcePath.parent_path();
  const std::wstring stem = sourcePath.stem().wstring();
  const std::wstring extension = sourcePath.extension().wstring();
  std::error_code error;
  for (uint32_t index = 1; index < 100000; ++index) {
    const std::wstring suffix =
        index == 1 ? L" - edited"
                   : L" - edited (" + std::to_wstring(index) + L")";
    const std::filesystem::path candidate =
        directory / std::filesystem::path(stem + suffix + extension);
    const bool exists = std::filesystem::exists(candidate, error);
    if (!error && !exists && candidate != sourcePath) return candidate;
    error.clear();
  }
  return {};
}

struct Exporter::Impl {
  explicit Impl(Operation exportOperation)
      : operation(std::move(exportOperation)) {}

  mutable std::mutex mutex;
  WaitableSignal changed;
  std::thread worker;
  std::atomic<bool> cancelled{false};
  Operation operation;
  ExportSnapshot state;
  std::optional<ExportSnapshot> completion;
  std::chrono::steady_clock::time_point lastProgressNotification =
      std::chrono::steady_clock::time_point::min();

  void updateProgress(double progress) {
    const auto now = std::chrono::steady_clock::now();
    bool notify = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (state.state != ExportState::Running) return;
      progress = std::clamp(progress, 0.0, 1.0);
      if (progress >= 1.0 || progress - state.progress >= 0.01 ||
          lastProgressNotification ==
              std::chrono::steady_clock::time_point::min() ||
          now - lastProgressNotification >= std::chrono::milliseconds(250)) {
        state.progress = progress;
        lastProgressNotification = now;
        notify = true;
      }
    }
    if (notify) changed.signal();
  }

  void run(ExportRequest request) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comResult);
    ExportResult completed;
    try {
      completed = operation(
          request, &cancelled,
          [this](double progress) { updateProgress(progress); });
    } catch (const std::exception& exception) {
      completed.state = ExportState::Failed;
      completed.error = exception.what();
    } catch (...) {
      completed.state = ExportState::Failed;
      completed.error = "Unexpected export error.";
    }
    if (uninitializeCom) CoUninitialize();
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (completed.state != ExportState::Succeeded &&
          cancelled.load(std::memory_order_relaxed)) {
        completed.state = ExportState::Cancelled;
        completed.error.clear();
      } else if (completed.state == ExportState::Idle ||
                 completed.state == ExportState::Running) {
        completed.state = ExportState::Failed;
        completed.error = "Export backend returned a non-terminal result.";
      }
      state.state = completed.state;
      state.progress = completed.state == ExportState::Succeeded ? 1.0
                                                                 : state.progress;
      state.videoEncoder = std::move(completed.videoEncoder);
      state.error = std::move(completed.error);
      completion = state;
    }
    changed.signal();
  }
};

Exporter::Exporter(Operation operation)
    : impl_(std::make_unique<Impl>(std::move(operation))) {}

Exporter::~Exporter() { stop(); }

bool Exporter::start(ExportRequest request) {
  if (!impl_ || request.sourcePath.empty() || request.destinationPath.empty() ||
      !validRanges(request.decisions.keptRanges) ||
      !request.decisions.hasValidShape()) {
    return false;
  }
  std::thread previous;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->operation || impl_->state.state == ExportState::Running ||
        impl_->completion) {
      return false;
    }
    if (impl_->worker.joinable()) previous = std::move(impl_->worker);
  }
  if (previous.joinable()) previous.join();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->cancelled.store(false, std::memory_order_relaxed);
    impl_->state = ExportSnapshot{};
    impl_->state.state = ExportState::Running;
    impl_->state.destinationPath = request.destinationPath;
    impl_->state.decisions = request.decisions;
    impl_->lastProgressNotification =
        std::chrono::steady_clock::time_point::min();
    try {
      impl_->worker = std::thread(
          [implementation = impl_.get(), request = std::move(request)]() mutable {
            implementation->run(std::move(request));
          });
    } catch (...) {
      impl_->state.state = ExportState::Failed;
      impl_->state.error = "Could not start the export worker.";
      impl_->completion = impl_->state;
      impl_->changed.signal();
      return false;
    }
  }
  impl_->changed.signal();
  return true;
}

bool Exporter::cancel() {
  if (!impl_) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->state.state != ExportState::Running ||
      impl_->cancelled.exchange(true, std::memory_order_relaxed)) {
    return false;
  }
  impl_->changed.signal();
  return true;
}

void Exporter::stop() {
  if (!impl_) return;
  impl_->cancelled.store(true, std::memory_order_relaxed);
  if (impl_->worker.joinable()) impl_->worker.join();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state.state == ExportState::Running) {
      impl_->state.state = ExportState::Cancelled;
      impl_->state.error.clear();
    }
  }
  impl_->changed.clear();
}

ExportSnapshot Exporter::snapshot() const {
  if (!impl_) return {};
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state;
}

std::optional<ExportSnapshot> Exporter::takeCompletion() {
  if (!impl_) return std::nullopt;
  std::optional<ExportSnapshot> completion;
  std::thread finishedWorker;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->completion) return std::nullopt;
    completion = std::move(impl_->completion);
    impl_->completion.reset();
    if (impl_->state.state != ExportState::Running &&
        impl_->worker.joinable()) {
      finishedWorker = std::move(impl_->worker);
    }
  }
  if (finishedWorker.joinable()) finishedWorker.join();
  return completion;
}

bool Exporter::consumeChanged() {
  return impl_ && impl_->changed.consume();
}

NativeWaitHandle Exporter::nativeWaitHandle() const {
  return impl_ ? impl_->changed.nativeWaitHandle() : NativeWaitHandle{};
}

}  // namespace playback_video_edit
