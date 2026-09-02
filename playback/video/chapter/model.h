#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "playback/video/chapter/backend.h"

namespace playback_video_chapters {

struct ModelPaths {
  std::filesystem::path directory;
  std::filesystem::path model;
  std::filesystem::path projector;
};

ModelPaths resolveModelPaths();
CapabilityResult inspectModelArtifacts(const ModelPaths& paths,
                                       const OperationControl& control);
InstallResult installModelArtifacts(const ModelPaths& paths,
                                    const OperationControl& control);

inline constexpr std::uintmax_t kModelBytes = 4683072032ull;
inline constexpr std::uintmax_t kProjectorBytes = 853119712ull;
inline constexpr std::uintmax_t kModelDownloadBytes =
    kModelBytes + kProjectorBytes;
inline constexpr const char* kModelSha256 =
    "9258bf05b12686d097ff3b6b18d968ab393649780aa2b3cd67fec43d50554392";
inline constexpr const char* kProjectorSha256 =
    "2ddb555391bae966e412deab9e07b58afa18bcc06930ba0f1c78a3695ab9e506";

}  // namespace playback_video_chapters
