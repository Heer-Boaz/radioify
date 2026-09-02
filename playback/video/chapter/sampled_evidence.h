#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "playback/video/chapter/backend.h"

namespace playback_video_chapters {

struct SampledFrame {
  std::int64_t timeUs = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgb;
};

struct SampledEvidenceResult {
  OperationStatus status = OperationStatus::Failed;
  std::string detail;
  std::vector<SampledFrame> frames;
};

OperationStatus probeHardwareVideoDecode(const AnalysisRequest& request,
                                         const OperationControl& control,
                                         std::string* detail = nullptr);
SampledEvidenceResult sampleVideoEvidence(const AnalysisRequest& request,
                                          const OperationControl& control);

}  // namespace playback_video_chapters
