#include "playback/video/analysis/evidence_preparation.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "ffmpegaudio.h"

#include "core/sha256.h"
#include "playback/video/analysis/inference_worker.h"
#include "playback/video/analysis/text_evidence.h"
#include "playback/video/transcript/artifact.h"
#include "playback/video/transcript/source_identity.h"
#include "playback/video/transcript/transcriber.h"

namespace playback_video_analysis {
namespace {

std::optional<TextEvidence> providedEvidence(const JobRequest &request) {
  if (!request.englishText || request.englishText->cues.empty())
    return std::nullopt;
  return *request.englishText;
}

} // namespace

EvidencePreparation::EvidencePreparation() = default;
EvidencePreparation::~EvidencePreparation() = default;

std::optional<TextEvidence>
EvidencePreparation::discover(const JobRequest &request,
                              std::string *detail) const {
  if (detail)
    detail->clear();
  if (auto provided = providedEvidence(request))
    return provided;
  return loadGeneratedEnglishTextEvidence(request.sourcePath, detail);
}

EvidencePreparationResult
EvidencePreparation::prepare(const JobRequest &request,
                             const OperationControl &control) {
  std::string discoveryDetail;
  if (auto existing = discover(request, &discoveryDetail))
    return {OperationStatus::Succeeded, {}, std::move(existing)};

  FfmpegAudioStreamFormat audio;
  if (!probeFfmpegAudioStream(request.sourcePath, &audio, nullptr))
    return {OperationStatus::Succeeded, {}, {}};

  const auto source =
      playback_video_transcript::captureTranscriptSourceIdentity(
          request.sourcePath);
  const std::string sourceKey =
      source ? core_sha256::text(
                   "speech\n" + std::to_string(source->instance.device) + "\n" +
                   std::to_string(source->instance.file) + "\n" +
                   std::to_string(source->size) + "\n" +
                   std::to_string(source->modified.time_since_epoch().count()))
             : std::string{};
  if (sourceKey.empty()) {
    reset();
    return {OperationStatus::Failed,
            "Could not establish a stable speech-evidence identity.",
            {}};
  }
  if (sourceKey_ != sourceKey) {
    reset();
    sourceKey_ = sourceKey;
    transcript_ = std::make_unique<
        playback_video_transcript::IndexedTranscriptOperation>();
  }

  if (!lease_) {
    InferenceWorkspaceLeaseResult acquired =
        acquireInferenceWorkspaceLease(sourceKey, control);
    if (acquired.status != OperationStatus::Succeeded) {
      if (acquired.status != OperationStatus::Yielded)
        reset();
      return {acquired.status, std::move(acquired.detail), {}};
    }
    lease_ = std::move(acquired.lease);
    // A different process may have published the prerequisite while this
    // request waited for exact-source ownership.
    if (auto existing = discover(request)) {
      discardInferenceWorkerWorkspace(lease_);
      lease_ = {};
      transcript_.reset();
      sourceKey_.clear();
      return {OperationStatus::Succeeded, {}, std::move(existing)};
    }
  }

  if (!transcript_) {
    transcript_ = std::make_unique<
        playback_video_transcript::IndexedTranscriptOperation>();
  }
  playback_video_transcript::TranscriptOperationControl transcriptControl;
  transcriptControl.cancelled = control.cancelled;
  transcriptControl.backgroundGpuAllowed = control.backgroundGpuAllowed;
  transcriptControl.progress =
      [publish = control.progress](
          const playback_video_transcript::Progress &progress) {
        if (publish) {
          publish(0.20 * std::clamp(static_cast<double>(progress.fraction), 0.0,
                                    1.0),
                  progress.phase);
        }
      };
  transcriptControl.runSpeechChunk =
      [this,
       &control](const playback_video_transcript::SpeechWorkerRequest &request,
                 const std::function<void(int)> &progress) {
        OperationControl workerControl = control;
        workerControl.progress = [progress](std::optional<double> fraction,
                                            const std::string &) {
          if (progress && fraction) {
            progress(static_cast<int>(
                std::lround(std::clamp(*fraction, 0.0, 1.0) * 100.0)));
          }
        };
        return runSpeechTranscriptionWorker(request, workerControl, lease_);
      };

  const auto result = transcript_->resume(
      request.sourcePath,
      playback_video_transcript::generatedEnglishTranscriptPathForVideo(
          request.sourcePath),
      playback_video_transcript::TranscriptPublishMode::ReplaceOwned,
      playback_video_transcript::TranscriptLanguageMode::TranslateToEnglish,
      transcriptControl,
      [cancelled = control.cancelled,
       gpuAllowed = control.backgroundGpuAllowed]() {
        return (!cancelled || !cancelled()) && (!gpuAllowed || gpuAllowed());
      });

  if (result.status !=
      playback_video_transcript::TranscriptOperationStatus::Succeeded) {
    OperationStatus status = OperationStatus::Failed;
    if (result.status ==
        playback_video_transcript::TranscriptOperationStatus::Yielded) {
      status = OperationStatus::Yielded;
    } else if (result.status == playback_video_transcript::
                                    TranscriptOperationStatus::Cancelled) {
      status = OperationStatus::Cancelled;
    } else if (result.status ==
               playback_video_transcript::TranscriptOperationStatus::NoSpeech) {
      status = OperationStatus::Succeeded;
    }
    if (status != OperationStatus::Yielded)
      reset();
    return {status,
            result.detail.empty()
                ? "Could not prepare timed English speech evidence."
                : result.detail,
            {}};
  }

  transcript_.reset();
  sourceKey_.clear();
  discardInferenceWorkerWorkspace(lease_);
  lease_ = {};
  std::string verificationDetail;
  auto prepared = discover(request, &verificationDetail);
  if (!prepared) {
    reset();
    return {OperationStatus::Failed,
            verificationDetail.empty()
                ? "The published transcript could not be read."
                : std::move(verificationDetail),
            {}};
  }
  return {OperationStatus::Succeeded, {}, std::move(prepared)};
}

void EvidencePreparation::reset() {
  transcript_.reset();
  discardInferenceWorkerWorkspace(lease_);
  lease_ = {};
  sourceKey_.clear();
}

} // namespace playback_video_analysis
