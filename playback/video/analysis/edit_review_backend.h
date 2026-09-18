#pragma once

#include "playback/video/analysis/scene_analysis_job.h"
#include "playback/video/analysis/scene_analyzer.h"

namespace playback_video_analysis {

bool reviewVideoForEditing(const JobRequest &request,
                           const AnalysisProgressCallback &progress,
                           const std::atomic<bool> *cancelled,
                           ReviewJobResult *result, std::string *error);

} // namespace playback_video_analysis
