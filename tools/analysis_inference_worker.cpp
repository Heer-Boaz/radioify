#include "playback/video/analysis/inference_worker.h"

#include <filesystem>

int wmain(int argc, wchar_t **argv) {
  if (argc != 2 || !argv || !argv[1] || !*argv[1])
    return 1;
  return playback_video_analysis::inferenceWorkerMain(
      std::filesystem::path(argv[1]));
}
