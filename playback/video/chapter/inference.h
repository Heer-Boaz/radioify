#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "playback/video/chapter/backend.h"

namespace playback_video_chapters {

struct InferenceRequest {
  std::filesystem::path model;
  std::filesystem::path projector;
  std::uint32_t imageWidth = 0;
  std::uint32_t imageHeight = 0;
  const std::vector<std::uint8_t>* imageRgb = nullptr;
  std::string prompt;
};

struct InferenceResult {
  OperationStatus status = OperationStatus::Failed;
  std::string detail;
  std::string json;
};

// Owns the process-wide llama.cpp backend lifecycle. The public boundary stays
// typed and independent of llama.cpp's experimental C structs; all third-party
// ownership is contained by the implementation.
class InferenceEngine final {
 public:
  InferenceEngine();
  ~InferenceEngine();

  InferenceEngine(const InferenceEngine&) = delete;
  InferenceEngine& operator=(const InferenceEngine&) = delete;

  CapabilityResult inspect(const OperationControl& control);
  InferenceResult run(const InferenceRequest& request,
                      const OperationControl& control);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_chapters
