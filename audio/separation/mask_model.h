#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "audio/separation/diagnostics.h"

namespace audio_separation {

class BanditMaskModel {
 public:
  BanditMaskModel();
  ~BanditMaskModel();

  BanditMaskModel(const BanditMaskModel&) = delete;
  BanditMaskModel& operator=(const BanditMaskModel&) = delete;

  bool initialize(const std::filesystem::path& modelPath,
                  DiagnosticReporter diagnostics,
                  std::string* error);
  bool run(const std::vector<float>& spectrogramRealImag,
           std::vector<float>* masksRealImag,
           const std::atomic<bool>* cancelRequested,
           std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace audio_separation
