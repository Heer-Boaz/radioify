#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "playback/video/chapter/backend.h"

namespace playback_video_chapters {

struct ContactSheetResult {
  OperationStatus status = OperationStatus::Failed;
  std::string detail;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgb;
  std::vector<std::int64_t> sampleTimesUs;
};

OperationStatus probeHardwareVideoDecode(const AnalysisRequest& request,
                                         const OperationControl& control,
                                         std::string* detail = nullptr);
ContactSheetResult buildContactSheet(const AnalysisRequest& request,
                                     const OperationControl& control);

}  // namespace playback_video_chapters
