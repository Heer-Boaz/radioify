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

inline constexpr std::uintmax_t kModelBytes = 1961506240ull;
inline constexpr std::uintmax_t kProjectorBytes = 866071872ull;
inline constexpr std::uintmax_t kPlannerModelBytes = 4920739232ull;
inline constexpr std::uintmax_t kModelDownloadBytes =
    kModelBytes + kProjectorBytes + kPlannerModelBytes;
inline constexpr const char *kModelSha256 =
    "0c4fc8a2a912ec24f1ec8210c603d245430ea824cf6bee491820b34faa3be89f";
inline constexpr const char *kProjectorSha256 =
    "79611c59b5ad5b0547256602e3fb546a3041bcf6db5058091b6bcaa31f3a1c95";
inline constexpr const char *kPlannerModelSha256 =
    "7b064f5842bf9532c91456deda288a1b672397a54fa729aa665952863033557c";
} // namespace playback_video_chapters
