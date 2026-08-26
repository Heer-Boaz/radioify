#include "playback/video/transcript/transcriber.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>

#include "runtime_helpers.h"

namespace {

int runSmoke(const std::filesystem::path& input,
             const std::filesystem::path& output) {
  namespace transcript = playback_video_transcript;
  std::atomic<bool> cancelRequested{false};
  std::mutex progressMutex;
  float lastFraction = 0.0f;
  int lastPercent = -1;
  std::string lastPhase;
  bool monotonicProgress = true;
  bool vulkanConfirmed = false;
  std::string error;
  const bool created = transcript::createIndexedTranscript(
      input, output, transcript::TranscriptPublishMode::CreateNew,
      [&](const transcript::Progress& progress) {
        std::lock_guard<std::mutex> lock(progressMutex);
        if (progress.fraction + 0.000001f < lastFraction) {
          monotonicProgress = false;
        }
        lastFraction = std::max(lastFraction, progress.fraction);
        if (progress.phase.rfind("Vulkan ready on ", 0) == 0) {
          vulkanConfirmed = true;
        }
        const int percent = static_cast<int>(progress.fraction * 100.0f);
        if (percent != lastPercent || progress.phase != lastPhase) {
          std::cout << percent << "% " << progress.phase << std::endl;
          lastPercent = percent;
          lastPhase = progress.phase;
        }
      },
      &cancelRequested, &error);
  if (!created) {
    std::cerr << "indexed_transcript_smoke: " << error << '\n';
    return EXIT_FAILURE;
  }
  if (!monotonicProgress) {
    std::cerr <<
        "indexed_transcript_smoke: progress moved backwards during the run\n";
    return EXIT_FAILURE;
  }
  if (!vulkanConfirmed) {
    std::cerr << "indexed_transcript_smoke: Vulkan backend was not confirmed\n";
    return EXIT_FAILURE;
  }
  std::cout << "indexed_transcript_smoke: PASS output="
            << toUtf8String(output) << '\n';
  return EXIT_SUCCESS;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  if (argc != 3) {
    std::cerr <<
        "Usage: indexed_transcript_smoke <input-media> <output.srt>\n";
    return 2;
  }
  return runSmoke(std::filesystem::path(argv[1]),
                  std::filesystem::path(argv[2]));
}
#else
int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr <<
        "Usage: indexed_transcript_smoke <input-media> <output.srt>\n";
    return 2;
  }
  return runSmoke(std::filesystem::path(argv[1]),
                  std::filesystem::path(argv[2]));
}
#endif
