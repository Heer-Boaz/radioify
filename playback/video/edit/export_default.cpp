#include "playback/video/edit/export.h"

#include <utility>

#include "playback/video/edit/export_pipeline.h"

namespace playback_video_edit {

Exporter::Exporter() : Exporter(WakeNotifier{}) {}

Exporter::Exporter(WakeNotifier ownerWake)
    : Exporter([](const ExportRequest& request,
                  const std::atomic<bool>* cancelled,
                  const ProgressReporter& reportProgress) {
        return detail::runExportPipeline(request, cancelled, reportProgress);
      },
      std::move(ownerWake)) {}

}  // namespace playback_video_edit
