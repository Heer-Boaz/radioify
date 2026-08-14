#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "playback/video/edit/export.h"

namespace playback_video_edit::detail {

struct PipelineResult {
  ExportState state = ExportState::Failed;
  std::string videoEncoder;
  std::string error;
};

PipelineResult runExportPipeline(
    const ExportRequest& request, std::atomic<bool>* cancelled,
    const std::function<void(double)>& reportProgress);

}  // namespace playback_video_edit::detail
