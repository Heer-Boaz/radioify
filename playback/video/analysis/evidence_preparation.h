#pragma once

#include <memory>
#include <optional>

#include "playback/video/analysis/inference_worker.h"
#include "playback/video/analysis/scene_analysis_job.h"

namespace playback_video_transcript {
class IndexedTranscriptOperation;
}

namespace playback_video_analysis {

struct EvidencePreparationResult {
  OperationStatus status = OperationStatus::Failed;
  std::string detail;
  std::optional<TextEvidence> evidence;
};

// Acquires complete timed-English evidence before editing analysis. Owns
// resumable speech extraction, worker lifetime and final SRT publication.
class EvidencePreparation {
public:
  EvidencePreparation();
  ~EvidencePreparation();

  EvidencePreparation(const EvidencePreparation &) = delete;
  EvidencePreparation &operator=(const EvidencePreparation &) = delete;

  std::optional<TextEvidence> discover(const JobRequest &request,
                                       std::string *detail = nullptr) const;
  EvidencePreparationResult prepare(const JobRequest &request,
                                    const OperationControl &control);
  void reset();

private:
  std::unique_ptr<playback_video_transcript::IndexedTranscriptOperation>
      transcript_;
  InferenceWorkspaceLease lease_;
  std::string sourceKey_;
};

} // namespace playback_video_analysis
