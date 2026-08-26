#include "playback/video/edit/export.h"

#include "playback/video/edit/export_pipeline.h"

namespace playback_video_edit {

Exporter::Exporter()
    : Exporter([](const ExportRequest& request,
                  const std::atomic<bool>* cancelled,
                  const ProgressReporter& reportProgress) {
        return detail::runExportPipeline(request, cancelled, reportProgress);
      }) {}

}  // namespace playback_video_edit
