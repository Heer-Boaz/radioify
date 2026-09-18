#pragma once

#include "playback/video/analysis/edit_review.h"
#include "playback/video/gpu/memory_budget.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "playback/video/analysis/operation.h"

namespace playback_video_analysis {

// Owns the process-wide llama.cpp backend lifecycle. The public boundary stays
// typed and independent of llama.cpp's experimental C structs; all third-party
// ownership is contained by the implementation.
class InferenceEngine final {
public:
  InferenceEngine();
  ~InferenceEngine();

  InferenceEngine(const InferenceEngine &) = delete;
  InferenceEngine &operator=(const InferenceEngine &) = delete;

  CapabilityResult inspect(const OperationControl &control);
  std::optional<playback_video_gpu::MemoryBudget> gpuMemoryBudget();
  playback_video_analysis::ReviewResult reviewVideo(
      const playback_video_analysis::ReviewRequest &request,
      const OperationControl &control,
      const playback_video_analysis::ReviewProgress &completed,
      const std::function<bool(const playback_video_analysis::ReviewProgress &,
                               std::string *)> &checkpoint);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace playback_video_analysis
