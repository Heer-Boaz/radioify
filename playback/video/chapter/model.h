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
  std::filesystem::path plannerDirectory;
  std::filesystem::path plannerModel;
  std::filesystem::path plannerAdapter;
};

ModelPaths resolveModelPaths();
CapabilityResult inspectModelArtifacts(const ModelPaths &paths,
                                       const OperationControl &control);
InstallResult installModelArtifacts(const ModelPaths &paths,
                                    const OperationControl &control);

inline constexpr std::uintmax_t kModelBytes = 4683072032ull;
inline constexpr std::uintmax_t kProjectorBytes = 853119712ull;
inline constexpr std::uintmax_t kPlannerModelBytes = 4920739232ull;
inline constexpr std::uintmax_t kPlannerAdapterBytes = 13640320ull;
inline constexpr std::uintmax_t kModelDownloadBytes =
    kModelBytes + kProjectorBytes + kPlannerModelBytes;
inline constexpr const char *kModelSha256 =
    "9258bf05b12686d097ff3b6b18d968ab393649780aa2b3cd67fec43d50554392";
inline constexpr const char *kProjectorSha256 =
    "2ddb555391bae966e412deab9e07b58afa18bcc06930ba0f1c78a3695ab9e506";
inline constexpr const char *kPlannerModelSha256 =
    "7b064f5842bf9532c91456deda288a1b672397a54fa729aa665952863033557c";
inline constexpr const char *kPlannerAdapterSha256 =
    "4e07a53dfa65e356e691716645d795697427c30dc983b198faf9e84d7eec6da";

} // namespace playback_video_chapters
