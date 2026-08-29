#include "audio/separation/job.h"
#include "audio/separation/operation.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "runtime_helpers.h"

namespace {

using Clock = std::chrono::steady_clock;

enum class SuspensionPoint {
  GpuProcessing,
  InitialModelLoad,
  InitialModelCompilation,
};

struct CommandLine {
  std::filesystem::path media;
  std::optional<std::filesystem::path> model;
  SuspensionPoint suspensionPoint = SuspensionPoint::GpuProcessing;
};

std::optional<std::string> environmentValue(const char* name) {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || !value || length <= 1) {
    std::free(value);
    return std::nullopt;
  }
  std::string result(value);
  std::free(value);
  return result;
#else
  const char* value = std::getenv(name);
  return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

bool setEnvironmentValue(const char* name,
                         const std::optional<std::string>& value) {
#ifdef _WIN32
  return _putenv_s(name, value ? value->c_str() : "") == 0;
#else
  return value ? setenv(name, value->c_str(), 1) == 0
               : unsetenv(name) == 0;
#endif
}

// A compile smoke must own a cold cache; otherwise a machine with a warm
// production cache silently skips the code path under test. Process-local
// LOCALAPPDATA isolation keeps the smoke hermetic without mutating a user's
// Radioify cache.
class ScopedColdCache {
 public:
  static std::unique_ptr<ScopedColdCache> create(std::string* error) {
    if (error) error->clear();
    std::error_code filesystemError;
    const std::filesystem::path temporaryRoot =
        std::filesystem::temp_directory_path(filesystemError);
    if (filesystemError || temporaryRoot.empty()) {
      if (error) {
        *error = "Could not locate temporary storage for the cold-cache "
                 "smoke.";
      }
      return nullptr;
    }

    std::filesystem::path root;
    const auto stamp = Clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      root = temporaryRoot /
             ("ras-" + std::to_string(stamp % 1000000000) + "-" +
              std::to_string(attempt));
      filesystemError.clear();
      if (std::filesystem::create_directory(root, filesystemError)) break;
      root.clear();
      if (filesystemError) break;
    }
    if (root.empty()) {
      if (error) {
        *error = "Could not create isolated storage for the cold-cache "
                 "smoke" +
                 (filesystemError ? ": " + filesystemError.message()
                                  : std::string("."));
      }
      return nullptr;
    }

    const std::optional<std::string> previous =
        environmentValue("LOCALAPPDATA");
    if (!setEnvironmentValue("LOCALAPPDATA", toUtf8String(root))) {
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
      if (error) {
        *error = "Could not isolate LOCALAPPDATA for the cold-cache smoke.";
      }
      return nullptr;
    }
    return std::unique_ptr<ScopedColdCache>(
        new ScopedColdCache(std::move(root), previous));
  }

  ~ScopedColdCache() {
    setEnvironmentValue("LOCALAPPDATA", previous_);
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  ScopedColdCache(const ScopedColdCache&) = delete;
  ScopedColdCache& operator=(const ScopedColdCache&) = delete;

 private:
  ScopedColdCache(std::filesystem::path root,
                  std::optional<std::string> previous)
      : root_(std::move(root)), previous_(std::move(previous)) {}

  std::filesystem::path root_;
  std::optional<std::string> previous_;
};

bool startsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

bool endsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

bool gpuReady(const audio_separation::JobSnapshot& snapshot) {
  return endsWith(snapshot.phase, " GPU ready");
}

bool initialModelLoadStarted(
    const audio_separation::JobSnapshot& snapshot) {
  const std::string_view phase(snapshot.phase);
  return startsWith(phase, "Loading ") &&
         endsWith(phase, " separation model");
}

bool initialModelCompilationStarted(
    const audio_separation::JobSnapshot& snapshot) {
  return snapshot.phase ==
         "Optimizing the NVIDIA separation model (first use)";
}

bool modelCompilationResumed(
    const audio_separation::JobSnapshot& snapshot) {
  return snapshot.phase ==
         "Resuming NVIDIA model optimization (first use)";
}

bool modelRestoreStarted(const audio_separation::JobSnapshot& snapshot) {
  const std::string_view phase(snapshot.phase);
  return startsWith(phase, "Restoring ") &&
         endsWith(phase, " separation model");
}

bool diagnosticContains(const std::filesystem::path& path,
                        std::string_view text) {
  if (path.empty()) return false;
  std::ifstream stream(path, std::ios::binary);
  const std::string contents{std::istreambuf_iterator<char>(stream),
                             std::istreambuf_iterator<char>()};
  return contents.find(text) != std::string::npos;
}

bool parseCommandLine(int argc, char** argv, CommandLine* commandLine) {
  if (!commandLine || argc < 2 || argc > 4) return false;
  commandLine->media = pathFromUtf8String(argv[1]);
  bool suspensionPointSpecified = false;
  for (int index = 2; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--suspend-during-load") {
      if (suspensionPointSpecified) return false;
      commandLine->suspensionPoint = SuspensionPoint::InitialModelLoad;
      suspensionPointSpecified = true;
      continue;
    }
    if (argument == "--suspend-during-compile") {
      if (suspensionPointSpecified) return false;
      commandLine->suspensionPoint =
          SuspensionPoint::InitialModelCompilation;
      suspensionPointSpecified = true;
      continue;
    }
    if (commandLine->model) return false;
    commandLine->model = pathFromUtf8String(argv[index]);
  }
  return commandLine->suspensionPoint !=
             SuspensionPoint::InitialModelCompilation ||
         !commandLine->model;
}

}  // namespace

int main(int argc, char** argv) {
  CommandLine commandLine;
  if (!parseCommandLine(argc, argv, &commandLine)) {
    std::cerr << "Usage: audio_separation_scheduler_smoke <media-file> "
                 "[model-file] [--suspend-during-load|"
                 "--suspend-during-compile]\n"
                 "The compile mode always uses the bundled production "
                 "model and an isolated cold cache.\n";
    return 2;
  }

  std::unique_ptr<ScopedColdCache> coldCache;
  if (commandLine.suspensionPoint ==
      SuspensionPoint::InitialModelCompilation) {
    std::string isolationError;
    coldCache = ScopedColdCache::create(&isolationError);
    if (!coldCache) {
      std::cerr << "Audio separation scheduler smoke failed: "
                << isolationError << '\n';
      return EXIT_FAILURE;
    }
  }

  audio_separation::Job::Operation operation;
  if (commandLine.model) {
    operation = audio_separation::makeModelOperation(*commandLine.model);
  } else {
    const audio_separation::OperationBinding production =
        audio_separation::resolveProductionOperation();
    if (!production.ready()) {
      std::cerr << "Audio separation scheduler smoke failed: "
                << production.detail() << '\n';
      return EXIT_FAILURE;
    }
    operation = production.operation();
  }
  if (!operation) {
    std::cerr << "Audio separation scheduler smoke failed: backend is not "
                 "configured.\n";
    return EXIT_FAILURE;
  }

  audio_separation::Job job(operation);
  if (!job.tryStart(commandLine.media)) {
    std::cerr << "Audio separation scheduler smoke failed: job did not "
                 "start.\n";
    return EXIT_FAILURE;
  }

  constexpr auto kOverallTimeout = std::chrono::minutes(3);
  constexpr auto kTransitionTimeout = std::chrono::seconds(30);
  const auto started = Clock::now();
  std::optional<Clock::time_point> suspensionRequestedAt;
  std::optional<Clock::time_point> resumeRequestedAt;
  bool suspended = false;
  bool recoveryStarted = false;
  bool recoveryCompleted = false;
  bool cancellationRequested = false;
  std::optional<audio_separation::JobSnapshot> completion;

  while (Clock::now() - started < kOverallTimeout) {
    const audio_separation::JobSnapshot snapshot = job.snapshot();
    bool reachedSuspensionPoint = false;
    switch (commandLine.suspensionPoint) {
      case SuspensionPoint::GpuProcessing:
        reachedSuspensionPoint = gpuReady(snapshot);
        break;
      case SuspensionPoint::InitialModelLoad:
        reachedSuspensionPoint = initialModelLoadStarted(snapshot);
        break;
      case SuspensionPoint::InitialModelCompilation:
        reachedSuspensionPoint = initialModelCompilationStarted(snapshot);
        break;
    }
    if (!suspensionRequestedAt && reachedSuspensionPoint) {
      if (!job.setPaused(true)) {
        std::cerr << "Scheduler smoke failed: suspension was rejected.\n";
        break;
      }
      suspensionRequestedAt = Clock::now();
      switch (commandLine.suspensionPoint) {
        case SuspensionPoint::GpuProcessing:
          std::cout << "Suspension requested during GPU processing.\n";
          break;
        case SuspensionPoint::InitialModelLoad:
          std::cout << "Suspension requested during initial model load.\n";
          break;
        case SuspensionPoint::InitialModelCompilation:
          std::cout
              << "Suspension requested during initial model compilation.\n";
          break;
      }
    }

    if (suspensionRequestedAt && !suspended &&
        snapshot.scheduling ==
            audio_separation::JobSchedulingState::Suspended) {
      suspended = true;
      const double seconds = std::chrono::duration<double>(
                                 Clock::now() - *suspensionRequestedAt)
                                 .count();
      std::cout << "GPU resources suspended in " << seconds << " s.\n";
      if (!job.setPaused(false)) {
        std::cerr << "Scheduler smoke failed: resume was rejected.\n";
        break;
      }
      resumeRequestedAt = Clock::now();
    }

    const bool compilationMode =
        commandLine.suspensionPoint ==
        SuspensionPoint::InitialModelCompilation;
    if (resumeRequestedAt && !recoveryStarted &&
        (compilationMode ? modelCompilationResumed(snapshot)
                         : modelRestoreStarted(snapshot))) {
      recoveryStarted = true;
      if (compilationMode) {
        std::cout << "Model compilation resumed.\n";
      } else {
        std::cout << "GPU resource restoration started.\n";
      }
    }
    if (!compilationMode && recoveryStarted && !recoveryCompleted &&
        gpuReady(snapshot)) {
      recoveryCompleted = true;
      std::cout << "GPU resource restoration completed.\n";
      cancellationRequested = job.requestCancel();
    }

    if (suspensionRequestedAt && !suspended &&
        Clock::now() - *suspensionRequestedAt > kTransitionTimeout) {
      std::cerr << "Scheduler smoke failed: suspension timed out.\n";
      break;
    }
    if (resumeRequestedAt &&
        !(compilationMode ? recoveryStarted : recoveryCompleted) &&
        Clock::now() - *resumeRequestedAt > kTransitionTimeout) {
      std::cerr << "Scheduler smoke failed: recovery timed out.\n";
      break;
    }

    completion = job.takeCompletion();
    if (completion) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (!completion) {
    job.requestCancel();
    job.cancelAndJoin();
    completion = job.takeCompletion();
  }

  const bool compilationMode =
      commandLine.suspensionPoint ==
      SuspensionPoint::InitialModelCompilation;
  const bool spuriousCacheRepair =
      completion &&
      diagnosticContains(completion->diagnosticLog,
                         "cached NVIDIA model could not be loaded");
  const bool passed =
      suspended && recoveryStarted && completion && !spuriousCacheRepair &&
      (compilationMode
           ? completion->state == audio_separation::JobState::Succeeded
           : recoveryCompleted && cancellationRequested &&
                 completion->state ==
                     audio_separation::JobState::Cancelled);
  if (!passed) {
    std::cerr << "Audio separation scheduler smoke failed.";
    if (spuriousCacheRepair) {
      std::cerr << " Cancellation was misclassified as cache corruption.";
    }
    if (completion && !completion->error.empty()) {
      std::cerr << ' ' << completion->error;
    }
    std::cerr << '\n';
    return EXIT_FAILURE;
  }

  std::cout << "Audio separation scheduler smoke passed.\n";
  if (!completion->diagnosticLog.empty()) {
    if (coldCache) {
      std::cout << "Temporary cold-cache diagnostics will be removed.\n";
    } else {
      std::cout << "Diagnostics: "
                << toUtf8String(completion->diagnosticLog) << '\n';
    }
  }
  return EXIT_SUCCESS;
}
