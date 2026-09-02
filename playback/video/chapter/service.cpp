#include "playback/video/chapter/service.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "core/waitable_signal.h"

namespace playback_video_chapters {
namespace {

Snapshot initialSnapshot(const AnalysisRequest& request) {
  Snapshot snapshot;
  snapshot.state = AnalysisState::CheckingSupport;
  snapshot.durationUs = request.durationUs;
  snapshot.phase = "Checking GPU and model";
  return snapshot;
}

}  // namespace

struct Service::Impl {
  explicit Impl(std::unique_ptr<Backend> implementation)
      : backend(std::move(implementation)), worker([this]() { run(); }) {}

  ~Impl() {
    stopping.store(true, std::memory_order_release);
    generation.fetch_add(1, std::memory_order_acq_rel);
    installCancelled.store(true, std::memory_order_release);
    condition.notify_all();
    if (worker.joinable()) worker.join();
  }

  struct ActiveRequest {
    RequestId id = 0;
    AnalysisRequest request;
  };

  std::unique_ptr<Backend> backend;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::optional<ActiveRequest> active;
  Snapshot published;
  std::uint64_t nextRequestId = 1;
  std::uint64_t nextRevision = 1;
  bool installRequested = false;
  bool installInProgress = false;
  std::atomic<bool> installCancelled{false};
  std::atomic<bool> stopping{false};
  std::atomic<std::uint64_t> generation{0};
  std::atomic<bool> gpuAllowed{false};
  WaitableSignal changed;
  std::thread worker;

  bool current(std::uint64_t expectedGeneration) const {
    return !stopping.load(std::memory_order_acquire) &&
           generation.load(std::memory_order_acquire) == expectedGeneration;
  }

  void publish(std::uint64_t expectedGeneration, Snapshot snapshot) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!active || !current(expectedGeneration)) return;
      snapshot.durationUs = active->request.durationUs;
      snapshot.revision = nextRevision++;
      published = std::move(snapshot);
    }
    changed.signal();
  }

  OperationControl analysisControl(std::uint64_t expectedGeneration) {
    OperationControl control;
    control.cancelled = [this, expectedGeneration]() {
      return !current(expectedGeneration);
    };
    control.backgroundGpuAllowed = [this, expectedGeneration]() {
      return current(expectedGeneration) &&
             gpuAllowed.load(std::memory_order_acquire);
    };
    control.progress = [this, expectedGeneration](
                           std::optional<double> progress,
                           std::string phase) {
      Snapshot snapshot;
      snapshot.state = AnalysisState::Analyzing;
      snapshot.progress = progress;
      snapshot.phase = std::move(phase);
      publish(expectedGeneration, std::move(snapshot));
    };
    return control;
  }

  OperationControl inspectionControl(std::uint64_t expectedGeneration) {
    OperationControl control = analysisControl(expectedGeneration);
    control.progress = [this, expectedGeneration](
                           std::optional<double> progress,
                           std::string phase) {
      Snapshot snapshot;
      snapshot.state = AnalysisState::CheckingSupport;
      snapshot.progress = progress;
      snapshot.phase = std::move(phase);
      publish(expectedGeneration, std::move(snapshot));
    };
    return control;
  }

  OperationControl installControl(std::uint64_t expectedGeneration) {
    OperationControl control;
    control.cancelled = [this]() {
      return stopping.load(std::memory_order_acquire) ||
             installCancelled.load(std::memory_order_acquire);
    };
    control.backgroundGpuAllowed = []() { return true; };
    control.progress = [this, expectedGeneration](
                           std::optional<double> progress,
                           std::string phase) {
      Snapshot snapshot;
      snapshot.state = AnalysisState::Installing;
      snapshot.progress = progress;
      snapshot.phase = std::move(phase);
      publish(expectedGeneration, std::move(snapshot));
    };
    return control;
  }

  bool waitForRequest(std::uint64_t* observedGeneration,
                      ActiveRequest* request) {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&]() {
      return stopping.load(std::memory_order_acquire) ||
             (active && generation.load(std::memory_order_acquire) !=
                            *observedGeneration) ||
             (active && installRequested);
    });
    if (stopping.load(std::memory_order_acquire)) return false;
    *observedGeneration = generation.load(std::memory_order_acquire);
    *request = *active;
    return true;
  }

  bool waitForInstallOrReplacement(std::uint64_t expectedGeneration) {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&]() {
      return stopping.load(std::memory_order_acquire) ||
             generation.load(std::memory_order_acquire) !=
                 expectedGeneration ||
             installRequested;
    });
    return !stopping.load(std::memory_order_acquire) &&
           generation.load(std::memory_order_acquire) == expectedGeneration;
  }

  bool waitForGpuOrReplacement(std::uint64_t expectedGeneration) {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&]() {
      return stopping.load(std::memory_order_acquire) ||
             generation.load(std::memory_order_acquire) !=
                 expectedGeneration ||
             gpuAllowed.load(std::memory_order_acquire);
    });
    return !stopping.load(std::memory_order_acquire) &&
           generation.load(std::memory_order_acquire) == expectedGeneration;
  }

  void publishTerminal(std::uint64_t expectedGeneration,
                       AnalysisState state, std::string detail) {
    Snapshot snapshot;
    snapshot.state = state;
    snapshot.detail = std::move(detail);
    publish(expectedGeneration, std::move(snapshot));
  }

  bool performInstallation(std::uint64_t expectedGeneration) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      installRequested = false;
      installInProgress = true;
      installCancelled.store(false, std::memory_order_release);
    }
    Snapshot installing;
    installing.state = AnalysisState::Installing;
    installing.phase = "Preparing model download";
    publish(expectedGeneration, std::move(installing));
    InstallResult result;
    try {
      result = backend->install(installControl(expectedGeneration));
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex);
      installInProgress = false;
      throw;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      installInProgress = false;
    }
    if (!current(expectedGeneration)) return false;
    if (result.status == OperationStatus::Succeeded) return true;
    if (result.status == OperationStatus::Cancelled) {
      publishTerminal(expectedGeneration, AnalysisState::SetupRequired,
                      result.detail.empty() ? "Model installation cancelled."
                                            : result.detail);
    } else {
      publishTerminal(expectedGeneration, AnalysisState::SetupRequired,
                      result.detail.empty() ? "Model installation failed."
                                            : result.detail);
    }
    return false;
  }

  void process(ActiveRequest request, std::uint64_t expectedGeneration) {
    if (request.request.file.empty() ||
        request.request.videoStreamIndex < 0 ||
        request.request.durationUs <= 0) {
      publishTerminal(
          expectedGeneration, AnalysisState::Unsupported,
          "Automatic chapters require a seekable video with a known "
          "duration.");
      return;
    }
    for (;;) {
      if (!current(expectedGeneration)) return;
      if (std::optional<AnalysisResult> cached =
              backend->cached(request.request)) {
        std::string validationError;
        if (cached->status == OperationStatus::Succeeded &&
            validateAutomaticAnalysis(request.request.durationUs,
                                      cached->overview, cached->chapters,
                                      &validationError)) {
          Snapshot ready;
          ready.state = AnalysisState::Ready;
          ready.overview = std::move(cached->overview);
          ready.chapters = std::move(cached->chapters);
          publish(expectedGeneration, std::move(ready));
          return;
        }
      }
      if (!gpuAllowed.load(std::memory_order_acquire)) {
        Snapshot waiting;
        waiting.state = AnalysisState::WaitingForPlayback;
        waiting.phase = "Waiting for stable playback";
        publish(expectedGeneration, std::move(waiting));
        if (!waitForGpuOrReplacement(expectedGeneration)) return;
      }
      Snapshot checking = initialSnapshot(request.request);
      publish(expectedGeneration, std::move(checking));
      const CapabilityResult capability =
          backend->inspect(request.request,
                           inspectionControl(expectedGeneration));
      if (!current(expectedGeneration) ||
          capability.state == CapabilityState::Cancelled) {
        return;
      }
      if (capability.state == CapabilityState::Unsupported) {
        publishTerminal(expectedGeneration, AnalysisState::Unsupported,
                        capability.detail);
        return;
      }
      if (capability.state == CapabilityState::Yielded) {
        Snapshot waiting;
        waiting.state = AnalysisState::WaitingForPlayback;
        waiting.phase = "Playback has GPU priority";
        publish(expectedGeneration, std::move(waiting));
        if (!waitForGpuOrReplacement(expectedGeneration)) return;
        continue;
      }
      if (capability.state == CapabilityState::SetupRequired) {
        publishTerminal(expectedGeneration, AnalysisState::SetupRequired,
                        capability.detail);
        if (!waitForInstallOrReplacement(expectedGeneration)) return;
        bool requested = false;
        {
          std::lock_guard<std::mutex> lock(mutex);
          requested = installRequested;
        }
        if (!requested || !performInstallation(expectedGeneration)) return;
        continue;
      }

      if (!gpuAllowed.load(std::memory_order_acquire)) {
        Snapshot waiting;
        waiting.state = AnalysisState::WaitingForPlayback;
        waiting.phase = "Waiting for stable playback";
        publish(expectedGeneration, std::move(waiting));
        if (!waitForGpuOrReplacement(expectedGeneration)) return;
      }

      AnalysisResult result =
          backend->analyze(request.request,
                           analysisControl(expectedGeneration));
      if (!current(expectedGeneration) ||
          result.status == OperationStatus::Cancelled) {
        return;
      }
      if (result.status == OperationStatus::Yielded) {
        Snapshot waiting;
        waiting.state = AnalysisState::WaitingForPlayback;
        waiting.phase = "Playback has GPU priority";
        publish(expectedGeneration, std::move(waiting));
        if (!waitForGpuOrReplacement(expectedGeneration)) return;
        continue;
      }
      if (result.status == OperationStatus::Unsupported) {
        publishTerminal(expectedGeneration, AnalysisState::Unsupported,
                        result.detail);
        return;
      }
      if (result.status != OperationStatus::Succeeded) {
        publishTerminal(expectedGeneration, AnalysisState::Failed,
                        result.detail.empty() ? "Chapter analysis failed."
                                              : result.detail);
        return;
      }

      std::string validationError;
      if (!validateAutomaticAnalysis(request.request.durationUs,
                                     result.overview, result.chapters,
                                     &validationError)) {
        publishTerminal(expectedGeneration, AnalysisState::Failed,
                        validationError);
        return;
      }
      Snapshot ready;
      ready.state = AnalysisState::Ready;
      ready.overview = std::move(result.overview);
      ready.chapters = std::move(result.chapters);
      publish(expectedGeneration, std::move(ready));
      return;
    }
  }

  void run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::uint64_t observedGeneration = 0;
    for (;;) {
      ActiveRequest request;
      if (!waitForRequest(&observedGeneration, &request)) return;
      try {
        process(std::move(request), observedGeneration);
      } catch (const std::exception& error) {
        publishTerminal(observedGeneration, AnalysisState::Failed,
                        "Chapter analysis failed: " +
                            std::string(error.what()));
      } catch (...) {
        publishTerminal(observedGeneration, AnalysisState::Failed,
                        "Chapter analysis failed unexpectedly.");
      }
    }
  }
};

Service::Service(std::unique_ptr<Backend> backend)
    : impl_(std::make_unique<Impl>(std::move(backend))) {}

Service::~Service() = default;

Service::RequestId Service::start(AnalysisRequest request) {
  RequestId id = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->installInProgress) {
      impl_->installCancelled.store(true, std::memory_order_release);
    }
    id = impl_->nextRequestId++;
    if (id == 0) id = impl_->nextRequestId++;
    impl_->active = Impl::ActiveRequest{id, std::move(request)};
    impl_->published = initialSnapshot(impl_->active->request);
    impl_->published.revision = impl_->nextRevision++;
    impl_->gpuAllowed.store(false, std::memory_order_release);
    impl_->installRequested = false;
    impl_->generation.fetch_add(1, std::memory_order_acq_rel);
  }
  impl_->condition.notify_all();
  impl_->changed.signal();
  return id;
}

void Service::cancel(RequestId requestId) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->active || impl_->active->id != requestId) return;
    if (impl_->installInProgress) {
      impl_->installCancelled.store(true, std::memory_order_release);
    }
    impl_->active.reset();
    impl_->generation.fetch_add(1, std::memory_order_acq_rel);
    impl_->gpuAllowed.store(false, std::memory_order_release);
  }
  impl_->condition.notify_all();
}

Snapshot Service::snapshot(RequestId requestId) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->active || impl_->active->id != requestId) {
    Snapshot unavailable;
    unavailable.state = AnalysisState::Unsupported;
    unavailable.detail = "No active video chapter request.";
    return unavailable;
  }
  return impl_->published;
}

bool Service::requestInstallation(RequestId requestId) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->active || impl_->active->id != requestId ||
        impl_->published.state != AnalysisState::SetupRequired ||
        impl_->installInProgress) {
      return false;
    }
    impl_->installRequested = true;
    impl_->installCancelled.store(false, std::memory_order_release);
  }
  impl_->condition.notify_all();
  return true;
}

bool Service::cancelInstallation(RequestId requestId) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->active || impl_->active->id != requestId ||
      !impl_->installInProgress) {
    return false;
  }
  impl_->installCancelled.store(true, std::memory_order_release);
  impl_->condition.notify_all();
  return true;
}

bool Service::retry(RequestId requestId) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->active || impl_->active->id != requestId ||
        impl_->published.state != AnalysisState::Failed ||
        impl_->installInProgress) {
      return false;
    }
    impl_->published = initialSnapshot(impl_->active->request);
    impl_->published.revision = impl_->nextRevision++;
    impl_->generation.fetch_add(1, std::memory_order_acq_rel);
  }
  impl_->condition.notify_all();
  impl_->changed.signal();
  return true;
}

void Service::setBackgroundGpuAllowed(RequestId requestId, bool allowed) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->active || impl_->active->id != requestId) return;
    impl_->gpuAllowed.store(allowed, std::memory_order_release);
  }
  impl_->condition.notify_all();
}

NativeWaitHandle Service::changedWaitHandle() const {
  return impl_->changed.nativeWaitHandle();
}

bool Service::consumeChanged() { return impl_->changed.consume(); }

}  // namespace playback_video_chapters
