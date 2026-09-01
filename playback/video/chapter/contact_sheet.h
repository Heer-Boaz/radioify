#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "playback/video/chapter/backend.h"

namespace playback_video_chapters {

struct ContactSheetResult {
  OperationStatus status = OperationStatus::Failed;
  std::string detail;
  std::filesystem::path pngPath;
  std::vector<std::int64_t> sampleTimesUs;
};

OperationStatus probeHardwareVideoDecode(const AnalysisRequest& request,
                                         const OperationControl& control,
                                         std::string* detail = nullptr);
ContactSheetResult buildContactSheet(const AnalysisRequest& request,
                                     const OperationControl& control);
void removeContactSheet(const std::filesystem::path& path);

}  // namespace playback_video_chapters
