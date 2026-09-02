#include "playback/video/chapter/service.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace playback_video_chapters;

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "chapter_service_tests: " << message << '\n';
    return false;
  }
  return true;
}

AnalysisResult validResult(std::int64_t durationUs) {
  AnalysisResult result;
  result.status = OperationStatus::Succeeded;
  result.overview = "The video has three sections.";
  result.chapters = {
      {1, 0, durationUs / 3, "Opening", "The introduction begins."},
      {2, durationUs / 3, 2 * durationUs / 3, "Middle",
       "The subject develops."},
      {3, 2 * durationUs / 3, durationUs, "Conclusion",
       "The result concludes."},
  };
  return result;
}

template <typename Predicate>
bool waitUntil(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return predicate();
}

class CachedBackend final : public Backend {
 public:
  std::optional<AnalysisResult> cached(
      const AnalysisRequest& request) override {
    return validResult(request.durationUs);
  }
  CapabilityResult inspect(const AnalysisRequest&,
                           const OperationControl&) override {
    ++inspectCalls;
    return {CapabilityState::Unsupported, "must not inspect"};
  }
  InstallResult install(const OperationControl&) override { return {}; }
  AnalysisResult analyze(const AnalysisRequest&,
                         const OperationControl&) override {
    ++analyzeCalls;
    return {};
  }

  std::atomic<int> inspectCalls{0};
  std::atomic<int> analyzeCalls{0};
};

class SetupBackend final : public Backend {
 public:
  std::optional<AnalysisResult> cached(const AnalysisRequest&) override {
    return std::nullopt;
  }
  CapabilityResult inspect(const AnalysisRequest&,
                           const OperationControl&) override {
    ++inspectCalls;
    return installed.load() ? CapabilityResult{CapabilityState::Ready, {}}
                            : CapabilityResult{
                                  CapabilityState::SetupRequired, "Install"};
  }
  InstallResult install(const OperationControl&) override {
    installed.store(true);
    return {OperationStatus::Succeeded, {}};
  }
  AnalysisResult analyze(const AnalysisRequest& request,
                         const OperationControl& control) override {
    ++analyzeCalls;
    if (!control.backgroundGpuAllowed()) {
      return {OperationStatus::Yielded, "foreground", {}, {}};
    }
    return validResult(request.durationUs);
  }

  std::atomic<bool> installed{false};
  std::atomic<int> inspectCalls{0};
  std::atomic<int> analyzeCalls{0};
};

class YieldBackend final : public Backend {
 public:
  std::optional<AnalysisResult> cached(const AnalysisRequest&) override {
    return std::nullopt;
  }
  CapabilityResult inspect(const AnalysisRequest&,
                           const OperationControl&) override {
    return {CapabilityState::Ready, {}};
  }
  InstallResult install(const OperationControl&) override { return {}; }
  AnalysisResult analyze(const AnalysisRequest& request,
                         const OperationControl& control) override {
    const int attempt = ++attempts;
    if (attempt == 1) {
      firstAttemptEntered.store(true);
      while (control.backgroundGpuAllowed() && !control.cancelled()) {
        std::this_thread::sleep_for(2ms);
      }
      return {control.cancelled() ? OperationStatus::Cancelled
                                  : OperationStatus::Yielded,
              {}, {}, {}};
    }
    return validResult(request.durationUs);
  }

  std::atomic<int> attempts{0};
  std::atomic<bool> firstAttemptEntered{false};
};

class FailOnceBackend final : public Backend {
 public:
  std::optional<AnalysisResult> cached(const AnalysisRequest&) override {
    return std::nullopt;
  }
  CapabilityResult inspect(const AnalysisRequest&,
                           const OperationControl&) override {
    return {CapabilityState::Ready, {}};
  }
  InstallResult install(const OperationControl&) override { return {}; }
  AnalysisResult analyze(const AnalysisRequest& request,
                         const OperationControl&) override {
    if (++attempts == 1) {
      return {OperationStatus::Failed, "invalid structured output", {}, {}};
    }
    return validResult(request.durationUs);
  }

  std::atomic<int> attempts{0};
};

class CancellableInstallBackend final : public Backend {
 public:
  std::optional<AnalysisResult> cached(const AnalysisRequest&) override {
    return std::nullopt;
  }
  CapabilityResult inspect(const AnalysisRequest&,
                           const OperationControl&) override {
    return {CapabilityState::SetupRequired, "Install"};
  }
  InstallResult install(const OperationControl& control) override {
    installEntered.store(true);
    while (!control.cancelled()) std::this_thread::sleep_for(2ms);
    cancellationObserved.store(true);
    return {OperationStatus::Cancelled, "cancelled"};
  }
  AnalysisResult analyze(const AnalysisRequest&,
                         const OperationControl&) override {
    return {};
  }

  std::atomic<bool> installEntered{false};
  std::atomic<bool> cancellationObserved{false};
};

AnalysisRequest request() {
  AnalysisRequest request;
  request.file = "movie.mkv";
  request.videoStreamIndex = 0;
  request.durationUs = 60'000'000;
  request.sourceWidth = 1920;
  request.sourceHeight = 1080;
  return request;
}

bool runCachedResultTest() {
  auto backend = std::make_unique<CachedBackend>();
  CachedBackend* observed = backend.get();
  Service service(std::move(backend));
  const Service::RequestId id = service.start(request());
  const bool ready = waitUntil([&]() { return service.snapshot(id).ready(); });
  return expect(ready && observed->inspectCalls.load() == 0 &&
                    observed->analyzeCalls.load() == 0,
                "a durable cache hit must bypass model and GPU work");
}

bool runExplicitSetupTest() {
  auto backend = std::make_unique<SetupBackend>();
  SetupBackend* observed = backend.get();
  Service service(std::move(backend));
  const Service::RequestId id = service.start(request());
  bool ok = expect(waitUntil([&]() {
                     return service.snapshot(id).state ==
                            AnalysisState::WaitingForPlayback;
                   }) &&
                       observed->inspectCalls.load() == 0,
                   "hardware inspection must wait for foreground GPU ownership");
  service.setBackgroundGpuAllowed(id, true);
  ok &= expect(waitUntil([&]() {
                     return service.snapshot(id).state ==
                            AnalysisState::SetupRequired;
                   }),
                   "missing models must wait for explicit setup");
  ok &= expect(observed->analyzeCalls.load() == 0,
               "setup-required state must not begin inference");
  ok &= expect(service.requestInstallation(id),
               "the active request must be able to approve installation");
  ok &= expect(waitUntil([&]() { return service.snapshot(id).ready(); }),
               "successful explicit setup must resume automatic analysis");
  return ok;
}

bool runInvalidSourceTest() {
  auto backend = std::make_unique<SetupBackend>();
  SetupBackend* observed = backend.get();
  Service service(std::move(backend));
  AnalysisRequest invalid = request();
  invalid.durationUs = 0;
  const Service::RequestId id = service.start(std::move(invalid));
  return expect(waitUntil([&]() {
                  return service.snapshot(id).state ==
                         AnalysisState::Unsupported;
                }) &&
                    observed->inspectCalls.load() == 0,
                "an unseekable source must fail before GPU or model setup");
}

bool runPlaybackYieldTest() {
  auto backend = std::make_unique<YieldBackend>();
  YieldBackend* observed = backend.get();
  Service service(std::move(backend));
  const Service::RequestId id = service.start(request());
  service.setBackgroundGpuAllowed(id, true);
  bool ok = expect(waitUntil([&]() {
                     return observed->firstAttemptEntered.load();
                   }),
                   "analysis must begin once the owner grants the GPU");
  service.setBackgroundGpuAllowed(id, false);
  ok &= expect(waitUntil([&]() {
                   return service.snapshot(id).state ==
                          AnalysisState::WaitingForPlayback;
                 }),
               "foreground playback must force analysis into a waiting state");
  service.setBackgroundGpuAllowed(id, true);
  ok &= expect(waitUntil([&]() { return service.snapshot(id).ready(); }) &&
                   observed->attempts.load() == 2,
               "yielded analysis must restart only after GPU ownership returns");
  return ok;
}

bool runExplicitRetryTest() {
  auto backend = std::make_unique<FailOnceBackend>();
  FailOnceBackend* observed = backend.get();
  Service service(std::move(backend));
  const Service::RequestId id = service.start(request());
  service.setBackgroundGpuAllowed(id, true);
  bool ok = expect(waitUntil([&]() {
                     return service.snapshot(id).state == AnalysisState::Failed;
                   }) &&
                       observed->attempts.load() == 1,
                   "a failed automatic analysis must publish a durable "
                   "terminal state");
  ok &= expect(service.retry(id),
               "the active failed request must expose an explicit retry");
  ok &= expect(waitUntil([&]() { return service.snapshot(id).ready(); }) &&
                   observed->attempts.load() == 2 && !service.retry(id),
               "retry must restart the same owner-bound request exactly once");
  return ok;
}

bool runRequestOwnedInstallTest() {
  auto backend = std::make_unique<CancellableInstallBackend>();
  CancellableInstallBackend* observed = backend.get();
  Service service(std::move(backend));
  const Service::RequestId id = service.start(request());
  service.setBackgroundGpuAllowed(id, true);
  bool ok = expect(waitUntil([&]() {
                     return service.snapshot(id).state ==
                            AnalysisState::SetupRequired;
                   }),
                   "request-owned setup must become available");
  ok &= expect(service.requestInstallation(id) &&
                   waitUntil([&]() { return observed->installEntered.load(); }),
               "the approved model install must start");
  service.cancel(id);
  ok &= expect(waitUntil(
                   [&]() { return observed->cancellationObserved.load(); }),
               "closing the active request must cancel its model install "
               "instead of leaving an invisible background download");
  return ok;
}

}  // namespace

int main() {
  return runCachedResultTest() && runInvalidSourceTest() &&
                 runExplicitSetupTest() && runPlaybackYieldTest() &&
                 runExplicitRetryTest() && runRequestOwnedInstallTest()
             ? 0
             : 1;
}
