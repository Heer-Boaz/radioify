#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "playback/session/subtitle_loader.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "FAIL: " << message << '\n';
  return false;
}

struct OperationProbe {
  std::mutex mutex;
  std::condition_variable changed;
  std::string startedFile;
  bool cancellationObserved = false;

  std::optional<SubtitleManager>
  run(std::filesystem::path file,
      const SubtitleManager::CancellationCheck& cancelled) {
    const std::string name = file.filename().string();
    {
      std::lock_guard<std::mutex> lock(mutex);
      startedFile = name;
    }
    changed.notify_all();

    if (name == "replacement.srt") return SubtitleManager{};

    std::unique_lock<std::mutex> lock(mutex);
    while (!cancelled || !cancelled()) {
      changed.wait_for(lock, std::chrono::milliseconds(5));
    }
    cancellationObserved = true;
    lock.unlock();
    changed.notify_all();
    return std::nullopt;
  }

  bool waitForStarted(const std::string& file) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(2),
                            [&]() { return startedFile == file; });
  }

  bool waitForCancellation() {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(2),
                            [&]() { return cancellationObserved; });
  }
};

bool supersedingRequestCancelsOldWork() {
  OperationProbe probe;
  playback_session::SubtitleLoadService loader(
      [&probe](std::filesystem::path file,
               const SubtitleManager::CancellationCheck& cancelled) {
        return probe.run(std::move(file), cancelled);
      });

  const auto first = loader.start("stale.srt");
  bool ok = expect(first.has_value(), "first request should start");
  ok &= expect(probe.waitForStarted("stale.srt"),
               "worker should start the first request");

  const auto replacement = loader.start("replacement.srt");
  ok &= expect(replacement.has_value(), "replacement request should start");
  ok &= expect(first && !loader.active(*first),
               "superseded request ownership is retired immediately");
  ok &= expect(replacement &&
                   WaitForSingleObject(loader.waitHandle(*replacement).get(),
                                       2000) == WAIT_OBJECT_0,
               "replacement completion should signal the owner");
  std::optional<playback_session::SubtitleLoadService::Completion> completion =
      replacement ? loader.poll(*replacement) : std::nullopt;
  ok &= expect(completion.has_value(),
               "replacement completion should be available");
  ok &=
      expect(completion && replacement && completion->requestId == *replacement,
             "only the replacement request may publish");
  ok &= expect(completion && completion->subtitles.has_value(),
               "an empty subtitle set is still a successful result");
  ok &= expect(probe.waitForCancellation(),
               "the superseded operation should observe cancellation");
  return ok;
}

bool explicitCancellationRetiresTheRequest() {
  OperationProbe probe;
  playback_session::SubtitleLoadService loader(
      [&probe](std::filesystem::path file,
               const SubtitleManager::CancellationCheck& cancelled) {
        return probe.run(std::move(file), cancelled);
      });

  const auto request = loader.start("cancel.srt");
  bool ok = expect(request.has_value(), "cancellable request should start");
  ok &= expect(probe.waitForStarted("cancel.srt"),
               "worker should start the cancellable request");
  ok &= expect(request && loader.cancel(*request),
               "the active request should cancel");
  ok &= expect(request && !loader.active(*request),
               "cancel retires request ownership immediately");
  ok &= expect(request && !loader.cancel(*request),
               "a retired request must not cancel twice");
  ok &= expect(probe.waitForCancellation(),
               "the operation should observe explicit cancellation");
  ok &= expect(request && !loader.poll(*request).has_value(),
               "a cancelled request must not publish a completion");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= supersedingRequestCancelsOldWork();
  ok &= explicitCancellationRetiresTheRequest();
  if (!ok) return 1;
  std::cout << "subtitle_loader_tests passed\n";
  return 0;
}
