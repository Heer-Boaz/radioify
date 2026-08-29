#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "audio/separation/diagnostics.h"
#include "audio/separation/execution_control.h"
#include "audio/separation/inference_backend.h"

namespace audio_separation {

enum class ModelPreparationMode {
  ReuseOrCreate,
  Rebuild,
};

struct PreparedModel {
  std::filesystem::path modelPath;
  InferenceBackend backend;
  bool compiled = false;
  bool cacheHit = false;
};

using ModelPreparationReporter = std::function<void(std::string)>;

// Produces the effective model/backend pair for one inference session. The
// NVIDIA EP-context model is published atomically and keyed by the verified
// bundled model plus provider version. NVIDIA's runtime cache remains a
// separate, provider-validated per-user artifact.
bool prepareBundledModel(
    const std::filesystem::path& sourceModelPath, InferenceBackend backend,
    const DiagnosticReporter& diagnostics, const ExecutionControl& control,
    ModelPreparationMode mode,
    const ModelPreparationReporter& reportPreparation,
    PreparedModel* prepared, std::string* error);

}  // namespace audio_separation
