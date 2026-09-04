#pragma once

#include <memory>
#include <optional>

#include "playback/video/chapter/backend.h"
#include "playback/video/chapter/inference_worker.h"

namespace playback_video_transcript {
class IndexedTranscriptOperation;
}

namespace playback_video_chapters {

enum class EnglishEvidenceOrigin : std::uint8_t {
  SessionSubtitleTrack,
  DurableGeneratedTranscript,
};

struct PreparedEvidence {
  AnalysisRoute route = AnalysisRoute::SpeechGuidedCaptionsAndAsr;
  EnglishEvidenceOrigin origin =
      EnglishEvidenceOrigin::DurableGeneratedTranscript;
  TextEvidence englishText;
};

struct EvidencePreparationResult {
  OperationStatus status = OperationStatus::Failed;
  std::string detail;
  std::optional<PreparedEvidence> evidence;
};

// Owns acquisition of the timed-English prerequisite. It deliberately does
// not own video sampling or either language model; those are later stages of
// the selected analysis route. This prevents "audio exists" from being
// confused with "the route has usable speech evidence".
class EvidencePreparation {
public:
  EvidencePreparation();
  ~EvidencePreparation();

  EvidencePreparation(const EvidencePreparation &) = delete;
  EvidencePreparation &operator=(const EvidencePreparation &) = delete;

  std::optional<PreparedEvidence>
  discover(const AnalysisRequest &request, std::string *detail = nullptr) const;
  EvidencePreparationResult prepare(const AnalysisRequest &request,
                                    const OperationControl &control);
  void reset();

private:
  std::unique_ptr<playback_video_transcript::IndexedTranscriptOperation>
      transcript_;
  InferenceWorkspaceLease lease_;
  std::string sourceKey_;
};

} // namespace playback_video_chapters
