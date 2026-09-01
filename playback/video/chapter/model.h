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
  std::filesystem::path engine;
};

ModelPaths resolveModelPaths();
CapabilityResult inspectModelArtifacts(const ModelPaths& paths,
                                       const OperationControl& control);
InstallResult installModelArtifacts(const ModelPaths& paths,
                                    const OperationControl& control);

inline constexpr std::uintmax_t kModelBytes = 1927933984ull;
inline constexpr std::uintmax_t kProjectorBytes = 592523200ull;
inline constexpr std::uintmax_t kModelDownloadBytes =
    kModelBytes + kProjectorBytes;
inline constexpr const char* kModelSha256 =
    "c850ffa51b0708be8911766e1d35e8e71365e987c8efb2513a7f237baade074f";
inline constexpr const char* kProjectorSha256 =
    "ae07ea1facd07dd3230c4483b63e8cda96c6944ad2481f33d531f79e892dd024";

}  // namespace playback_video_chapters
