#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

// Owner-held checkpoint for independently sampled caption frames. A
// cooperative GPU yield retains every completely extracted frame without
// inventing a second visual-analysis or boundary-snapping pass.
struct SampledEvidenceCheckpoint {
  std::vector<ChapterEvidenceInterval> intervals;
  std::vector<SampledTemporalWindow> windows;
};

OperationStatus probeHardwareVideoDecode(const AnalysisRequest &request,
                                         const OperationControl &control,
                                         std::string *detail = nullptr);
SampledEvidenceResult
sampleVideoEvidence(const AnalysisRequest &request,
                    const OperationControl &control,
                    SampledEvidenceCheckpoint *checkpoint,
                    const std::vector<ChapterEvidenceInterval> &plan);

} // namespace playback_video_chapters
