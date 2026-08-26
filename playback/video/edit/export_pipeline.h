#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "playback/video/edit/export.h"

namespace playback_video_edit::detail {

ExportResult runExportPipeline(
    const ExportRequest& request, const std::atomic<bool>* cancelled,
    const std::function<void(double)>& reportProgress);

}  // namespace playback_video_edit::detail
