#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "audio/separation/diagnostics.h"

namespace audio_separation {

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
                  DiagnosticReporter diagnostics,
                  std::string* error);
  bool run(std::span<const float> spectrogramRealImag,
           std::span<const float>* masksRealImag,
           const std::atomic<bool>* cancelRequested,
           std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace audio_separation
