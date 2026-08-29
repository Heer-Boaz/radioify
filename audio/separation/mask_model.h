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

enum class MaskInferenceResult {
  Succeeded,
  Interrupted,
  Failed,
};

class BanditMaskModel {
 public:
  // A batch of one keeps DirectML's peak activation memory bounded. Stereo
  // channels are evaluated sequentially through the same reusable buffers.
  static constexpr std::size_t kBatchSize = 1;

  BanditMaskModel();
  ~BanditMaskModel();

  BanditMaskModel(const BanditMaskModel&) = delete;
  BanditMaskModel& operator=(const BanditMaskModel&) = delete;

  bool initialize(const std::filesystem::path& modelPath,
                  const InferenceBackend& backend,
                  DiagnosticReporter diagnostics,
                  std::string* error);
  MaskInferenceResult run(std::span<const float> spectrogramRealImag,
                          std::span<const float>* masksRealImag,
                          const ExecutionControl& control,
                          std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace audio_separation
