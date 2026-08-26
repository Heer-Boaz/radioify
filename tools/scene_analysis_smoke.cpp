#include "playback/video/analysis/scene_analyzer.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

extern "C" {
#include <libavutil/log.h>
}

namespace {

std::string timestamp(int64_t valueUs) {
  valueUs = std::max<int64_t>(0, valueUs);
  const int64_t totalSeconds = valueUs / 1'000'000;
  const int64_t hours = totalSeconds / 3600;
  const int64_t minutes = (totalSeconds % 3600) / 60;
  const int64_t seconds = totalSeconds % 60;
  char value[32];
  std::snprintf(value, sizeof(value), "%02lld:%02lld:%02lld",
                static_cast<long long>(hours),
                static_cast<long long>(minutes),
                static_cast<long long>(seconds));
  return value;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  av_log_set_level(AV_LOG_QUIET);
  if (argc < 2 || argc > 4) {
    std::cerr << "Usage: scene_analysis_smoke <video> [video-stream-index] "
                 "[--cache]\n";
    return 2;
  }
  const std::filesystem::path videoPath(argv[1]);
  int streamIndex = -1;
  bool allowCachedResult = false;
  for (int index = 2; index < argc; ++index) {
    if (std::wstring(argv[index]) == L"--cache") {
      allowCachedResult = true;
      continue;
    }
    try {
      streamIndex = std::stoi(argv[index]);
    } catch (...) {
      std::cerr << "Invalid scene-analysis option\n";
      return 2;
    }
  }

  std::atomic<bool> cancelled{false};
  playback_video_analysis::AnalysisResult result;
  std::string error;
  std::string previousPhase;
  int previousPercentage = -1;
  const bool succeeded = playback_video_analysis::analyzeVideoScenes(
      videoPath, streamIndex, 0,
      [&](const playback_video_analysis::AnalysisProgress& progress) {
        const int percentage = static_cast<int>(
            std::clamp(progress.fraction, 0.0, 1.0) * 100.0);
        if (progress.phase != previousPhase || percentage >= previousPercentage + 10) {
          std::cerr << percentage << "% " << progress.phase << '\n';
          previousPhase = progress.phase;
          previousPercentage = percentage;
        }
      },
      &cancelled, allowCachedResult, &result, &error);
  if (!succeeded) {
    std::cerr << "scene_analysis_smoke: " << error << '\n';
    return 1;
  }

  std::cout << "scene_analysis_smoke: duration="
            << timestamp(result.durationUs)
            << " samples=" << result.visualSampleCount
            << " transcript=" << (!result.transcriptPath.empty() ? "yes" : "no")
            << " suggestions=" << result.suggestions.size() << '\n';
  for (const auto& suggestion : result.suggestions) {
    const int confidence = static_cast<int>(
        std::clamp(suggestion.confidence, 0.0f, 1.0f) * 100.0f);
    std::cout << suggestion.id << '\t'
              << playback_video_analysis::sceneKindLabel(suggestion.kind)
              << '\t' << timestamp(suggestion.startUs) << '\t'
              << timestamp(suggestion.endUs) << '\t' << confidence << "%\t"
              << playback_video_analysis::sceneEvidenceSummary(suggestion)
              << "\tspeech=" << suggestion.evidence.speechRatio
              << " letterbox=" << suggestion.evidence.letterboxRatio
              << " dark=" << suggestion.evidence.darkRatio
              << " cuts=" << suggestion.evidence.sceneChangeRate
              << " motion=" << suggestion.evidence.visualMotion
              << " hud=" << suggestion.evidence.hudLikelihood
              << '\n';
  }
  return 0;
}
