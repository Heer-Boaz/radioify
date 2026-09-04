#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "playback/video/chapter/backend.h"
#include "playback/video/chapter/model_contract.h"

namespace playback_video_chapters {

struct ModelPaths {
  std::filesystem::path directory;
  std::filesystem::path model;
  std::filesystem::path projector;
  std::filesystem::path plannerDirectory;
  std::filesystem::path plannerModel;
  std::filesystem::path speechPlanAdapter;
  std::filesystem::path chapterPlanAdapter;
};

ModelPaths resolveModelPaths();
CapabilityResult inspectModelArtifacts(const ModelPaths &paths,
                                       const OperationControl &control);
// Verifies only immutable files that must travel beside the executable. User
// downloaded model files and GPU capability are deliberately outside this
// staged-runtime contract.
CapabilityResult inspectPackagedChapterRuntime();
InstallResult installModelArtifacts(const ModelPaths &paths,
                                    const OperationControl &control);

inline constexpr std::uintmax_t kModelBytes = 4681089344ull;
inline constexpr std::uintmax_t kProjectorBytes = 1044425152ull;
inline constexpr std::uintmax_t kPlannerModelBytes = 4920739232ull;
inline constexpr std::uintmax_t kModelDownloadBytes =
    kModelBytes + kProjectorBytes + kPlannerModelBytes;
inline constexpr const char *kModelSha256 =
    "3a4078d53b46f22989adbf998ce5a3fd090b6541f112d7e936eb4204a04100b1";
inline constexpr const char *kProjectorSha256 =
    "4485f68a0f1aa404c391e788ea88ea653c100d8e98fe572698f701e5809711fd";
inline constexpr const char *kPlannerModelSha256 =
    "7b064f5842bf9532c91456deda288a1b672397a54fa729aa665952863033557c";
} // namespace playback_video_chapters
