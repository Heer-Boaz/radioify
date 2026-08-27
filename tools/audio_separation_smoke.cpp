#include "audio/separation/artifact.h"
#include "audio/separation/job.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

#include "runtime_helpers.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: audio_separation_smoke <media-file>\n";
    return 2;
  }
  const std::filesystem::path media = pathFromUtf8String(argv[1]);
  const audio_separation::ArtifactPaths outputs =
      audio_separation::artifactPathsFor(media);
  std::atomic<bool> cancelRequested{false};
  int lastPercent = -1;
  const auto started = std::chrono::steady_clock::now();
  std::string error;
  const audio_separation::Job::Operation operation =
      audio_separation::Job::productionOperation();
  if (!operation) {
    std::cerr << "Audio separation failed: production operation is not "
                 "configured\n";
    return EXIT_FAILURE;
  }
  const bool succeeded = operation(
      media, outputs,
      [&](float fraction, const std::string& phase) {
        const int percent = static_cast<int>(fraction * 100.0f);
        if (percent == lastPercent) return;
        lastPercent = percent;
        std::cout << std::setw(3) << percent << "%  " << phase
                  << '\n';
      },
      &cancelRequested, &error);
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
