#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "audio/separation/diagnostics.h"
#include "audio/separation/execution_control.h"
#include "audio/separation/inference_backend.h"

namespace audio_separation {

struct MaskInferenceOutput {
  std::span<const float> masksRealImag;
};

using MaskInferenceResult = ControlledValueResult<MaskInferenceOutput>;

class BanditMaskModel {
 public:
  // A batch of one keeps DirectML's peak activation memory bounded. Stereo
  // channels are evaluated sequentially through the same reusable buffers.
  static constexpr std::size_t kBatchSize = 1;

  BanditMaskModel();
  ~BanditMaskModel();

  BanditMaskModel(const BanditMaskModel&) = delete;
  BanditMaskModel& operator=(const BanditMaskModel&) = delete;

  ControlledOperationResult initialize(
      const std::filesystem::path& modelPath,
      const InferenceBackend& backend, DiagnosticReporter diagnostics,
      const ExecutionControl* control = nullptr);
  MaskInferenceResult run(
      std::span<const float> spectrogramRealImag,
      const ExecutionControl& control);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace audio_separation
