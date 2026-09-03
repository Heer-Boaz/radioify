#include "playback/video/analysis/scene_analyzer.h"

#include <algorithm>
#include <utility>

#include "playback/video/analysis/scene_analysis_cache.h"
#include "playback/video/analysis/scene_transcript_evidence.h"
#include "playback/video/analysis/visual_timeline_scan.h"
#include "playback/video/transcript/artifact.h"

namespace playback_video_analysis {
namespace {

void setError(std::string *error, std::string message) {
  if (error)
    *error = std::move(message);
}

bool cancelled(const std::atomic<bool> *cancelRequested) {
  return cancelRequested && cancelRequested->load(std::memory_order_relaxed);
}

void report(const AnalysisProgressCallback &callback, double fraction,
            std::string phase) {
  if (!callback)
    return;
  callback({std::clamp(fraction, 0.0, 1.0), std::move(phase)});
}

} // namespace

bool analyzeVideoScenes(const std::filesystem::path &videoPath,
                        int videoStreamIndex, int64_t expectedDurationUs,
                        const AnalysisProgressCallback &onProgress,
                        const std::atomic<bool> *cancelRequested,
                        bool allowCachedResult, AnalysisResult *result,
                        std::string *error) {
  if (error)
    error->clear();
  if (result)
    *result = {};
  if (videoPath.empty() || !result) {
    setError(error, "Video path or analysis result is empty.");
    return false;
  }
  if (cancelled(cancelRequested)) {
    setError(error, "Segment detection cancelled.");
    return false;
  }

  const std::filesystem::path transcriptPath =
      playback_video_transcript::activeTranscriptPathForVideo(videoPath);
  std::vector<playback_video_transcript::Segment> transcriptSegments;
  std::string transcriptError;
  report(onProgress, 0.005, "Reading transcript evidence");
  if (!loadSceneTranscriptEvidence(transcriptPath, &transcriptSegments,
                                   &transcriptError)) {
    setError(error, std::move(transcriptError));
    return false;
  }
  if (allowCachedResult && expectedDurationUs > 0 &&
      loadCachedSceneAnalysis(videoPath, videoStreamIndex, expectedDurationUs,
                              transcriptPath, result)) {
    report(onProgress, 1.0, "Loaded cached segment suggestions");
    return true;
  }

  report(onProgress, 0.01, "Opening video for segment detection");
  VisualTimelineScanControl scanControl;
  scanControl.cancelled = [cancelRequested] {
    return cancelled(cancelRequested);
  };
  scanControl.progress = [&](double fraction) {
    report(onProgress, 0.03 + fraction * 0.87, "Scanning visual changes");
  };
  VisualTimelineScanResult scan = scanVisualTimeline(
      {videoPath, videoStreamIndex, expectedDurationUs, false}, scanControl);
  if (scan.status != VisualTimelineScanStatus::Succeeded) {
    setError(error, scan.detail.empty()
                        ? "Could not scan the complete video timeline."
                        : std::move(scan.detail));
    return false;
  }
  const int64_t durationUs = scan.durationUs;
  if (allowCachedResult && expectedDurationUs <= 0 &&
      loadCachedSceneAnalysis(videoPath, videoStreamIndex, durationUs,
                              transcriptPath, result)) {
    report(onProgress, 1.0, "Loaded cached segment suggestions");
    return true;
  }

  report(onProgress, 0.92, "Applying transcript evidence");
  const std::vector<SpeechActivity> speech =
      buildSpeechActivity(transcriptSegments);
  report(onProgress, 0.96, "Grouping detected segments");
  std::vector<SceneSuggestion> suggestions =
      buildSceneSuggestions(durationUs, scan.samples, speech);
  if (suggestions.empty()) {
    setError(error, "Segment detection produced no usable ranges.");
    return false;
  }

  result->durationUs = durationUs;
  result->visualSampleCount = scan.samples.size();
  result->transcriptPath =
      transcriptSegments.empty() ? std::filesystem::path{} : transcriptPath;
  result->suggestions = std::move(suggestions);
  storeCachedSceneAnalysis(videoPath, videoStreamIndex, durationUs,
                           transcriptPath, *result);
  report(onProgress, 1.0, "Segment detection complete");
  return true;
}

} // namespace playback_video_analysis
