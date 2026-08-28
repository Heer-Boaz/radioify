#include "audio/separation/artifact.h"
#include "audio/separation/job.h"
#include "audio/separation/operation.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

#include "runtime_helpers.h"

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::cerr << "Usage: audio_separation_smoke <media-file> [model-file]\n";
    return 2;
  }
  const std::filesystem::path media = pathFromUtf8String(argv[1]);
  const audio_separation::ArtifactPaths outputs =
      audio_separation::artifactPathsFor(media);
  std::atomic<bool> cancelRequested{false};
  int lastPercent = -1;
  const auto started = std::chrono::steady_clock::now();
  std::string error;
  const audio_separation::Job::Operation operation = argc == 3
      ? audio_separation::makeModelOperation(pathFromUtf8String(argv[2]))
      : audio_separation::makeProductionOperation();
  if (!operation) {
    std::cerr << "Audio separation failed: operation is not configured\n";
    return EXIT_FAILURE;
  }
  const audio_separation::ExecutionControl control(&cancelRequested);
  bool outputCommitStarted = false;
  bool succeeded = operation(
      media, outputs,
      [&](float fraction, const std::string& phase) {
        const int percent = static_cast<int>(fraction * 100.0f);
        if (percent == lastPercent) return;
        lastPercent = percent;
        std::cout << std::setw(3) << percent << "%  " << phase
                  << '\n';
      },
      [](DiagnosticLevel level, std::string_view component,
         std::string_view message) {
        const char* levelName = level == DiagnosticLevel::Error
                                    ? "error"
                                    : level == DiagnosticLevel::Warning
                                          ? "warning"
                                          : "info";
        std::cerr << '[' << levelName << ":" << component << "] "
                  << message << '\n';
      },
      control,
      [&]() {
        if (outputCommitStarted) return false;
        outputCommitStarted = true;
        return true;
      },
      &error);
  if (succeeded && !outputCommitStarted) {
    succeeded = false;
    error = "The backend did not claim its output commit boundary.";
  }
  const double elapsedSeconds = std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();
  if (!succeeded) {
    std::cerr << "Audio separation failed: " << error << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "Completed in " << std::fixed << std::setprecision(2)
            << elapsedSeconds << " seconds.\n";
  for (const std::filesystem::path& output : outputs) {
    std::cout << toUtf8String(output) << '\n';
  }
  return EXIT_SUCCESS;
}
