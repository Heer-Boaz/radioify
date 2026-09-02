#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "playback/video/chapter/backend.h"
#include "playback/video/chapter/generated_document.h"

namespace playback_video_chapters {

struct InferenceImage {
  std::uint32_t imageWidth = 0;
  std::uint32_t imageHeight = 0;
  const std::vector<std::uint8_t>* imageRgb = nullptr;
  std::int64_t timeUs = 0;
  std::string englishDialogue;
};

struct InferenceRequest {
  std::filesystem::path model;
  std::filesystem::path projector;
  std::int64_t durationUs = 0;
  std::vector<InferenceImage> images;
};

struct InferenceResult {
  OperationStatus status = OperationStatus::Failed;
  std::string detail;
  GeneratedDocument document;
};

// Caller-owned, model-independent checkpoint for one exact inference request.
// Successfully validated stages survive cooperative GPU preemption; native
// llama/mtmd objects never cross a run() boundary and therefore release all
// GPU allocations immediately when foreground playback reclaims the device.
struct InferenceCheckpoint {
  std::filesystem::path model;
  std::filesystem::path projector;
  std::int64_t durationUs = 0;
  std::vector<std::int64_t> sampleTimesUs;
  std::vector<std::string> observations;
  std::optional<GeneratedSegmentationPlan> plan;
  std::vector<GeneratedChangePointScore> changePoints;
  std::vector<std::size_t> startFrames;
  std::vector<GeneratedChapterMetadata> chapterMetadata;
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
                      const OperationControl& control,
                      InferenceCheckpoint* checkpoint = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_chapters
