#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "playback/video/analysis/visual_timeline_scan.h"
#include "playback/video/chapter/backend.h"
#include "playback/video/chapter/evidence_plan.h"

namespace playback_video_chapters {

struct SampledFrame {
  std::int64_t timeUs = 0;
  std::int64_t intervalStartUs = 0;
  std::int64_t intervalEndUs = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgb;
};

struct SampledTemporalWindow {
  std::int64_t startUs = 0;
  std::int64_t endUs = 0;
  std::vector<SampledFrame> frames;
};

struct SampledEvidenceResult {
  OperationStatus status = OperationStatus::Failed;
  std::string detail;
};

// Owner-held checkpoint for the comparatively expensive two-pass evidence
// preparation. A cooperative GPU yield after the dense scan retains both the
// temporal plan and every completely extracted multi-frame window.
struct SampledEvidenceCheckpoint {
  playback_video_analysis::VisualTimelineScanCheckpoint timelineScan;
  std::vector<ChapterEvidenceInterval> intervals;
  std::vector<SampledTemporalWindow> windows;
};

OperationStatus probeHardwareVideoDecode(const AnalysisRequest &request,
                                         const OperationControl &control,
                                         std::string *detail = nullptr);
SampledEvidenceResult
sampleVideoEvidence(const AnalysisRequest &request,
                    const OperationControl &control,
                    SampledEvidenceCheckpoint *checkpoint);

} // namespace playback_video_chapters
