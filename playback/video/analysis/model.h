#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "playback/video/analysis/operation.h"

namespace playback_video_analysis {

struct VideoReviewModelPaths {
  std::filesystem::path model;
  std::filesystem::path projector;
};

// Vision uses Qwen3-VL Instruct's published profile. Archive segmentation and
// classification select the most likely token without diversity penalties.
struct QwenSampling {
  int topK;
  float topP;
  float temperature;
  float repeatPenalty;
  float frequencyPenalty;
  float presencePenalty;
  uint32_t seed;
};
inline constexpr QwenSampling kQwenVisionSampling{20,   0.8f, 0.7f, 1.0f,
                                                  0.0f, 1.5f, 42};
inline constexpr QwenSampling kQwenTextSampling{1,    1.0f, 0.0f, 1.0f,
                                                0.0f, 0.0f, 42};
VideoReviewModelPaths resolveVideoReviewModelPaths();
InstallResult installVideoReviewModel(const VideoReviewModelPaths &paths,
                                      const OperationControl &control);
inline constexpr const char *kVideoReviewModelSha256 =
    "67d1659bfe71b89d50b45a4ad1a9e5b997e5bb16ce5da66a6a6167abd569e9e2";
inline constexpr const char *kVideoReviewProjectorSha256 =
    "ca524100ebf825c9a870db1c580d03879e0da0ab2541697e2458e64891cf9d38";

} // namespace playback_video_analysis
