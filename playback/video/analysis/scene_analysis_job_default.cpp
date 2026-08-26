#include "playback/video/analysis/scene_analysis_job.h"

#include "playback/video/analysis/scene_analyzer.h"

namespace playback_video_analysis {

SceneAnalysisJob::SceneAnalysisJob()
    : SceneAnalysisJob(
          [](const JobRequest& request,
             const ProgressReporter& reportProgress,
             const std::atomic<bool>* cancelled, AnalysisResult* result,
             std::string* error) {
            return analyzeVideoScenes(
                request.sourcePath, request.videoStreamIndex,
                request.durationUs, reportProgress, cancelled,
                !request.forceReanalysis, result, error);
          }) {}

}  // namespace playback_video_analysis
