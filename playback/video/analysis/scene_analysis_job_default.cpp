#include "playback/video/analysis/scene_analysis_job.h"

#include <utility>

#include "playback/video/analysis/edit_review_backend.h"

namespace playback_video_analysis {

SceneAnalysisJob::SceneAnalysisJob() : SceneAnalysisJob(WakeNotifier{}) {}

SceneAnalysisJob::SceneAnalysisJob(WakeNotifier ownerWake)
    : SceneAnalysisJob(
          [](const JobRequest& request,
             const ProgressReporter& reportProgress,
             const std::atomic<bool>* cancelled, ReviewJobResult* result,
             std::string* error) {
            return reviewVideoForEditing(request, reportProgress, cancelled, result, error);
          },
          std::move(ownerWake)) {}

}  // namespace playback_video_analysis
