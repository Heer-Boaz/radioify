#include "playback/video/chapter/backend.h"

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

#include "core/utf8.h"
#include "playback/video/chapter/cache.h"
#include "playback/video/chapter/generated_document.h"
#include "playback/video/chapter/inference.h"
#include "playback/video/chapter/inference_worker.h"
#include "playback/video/chapter/model.h"
#include "playback/video/chapter/sampled_evidence.h"

namespace playback_video_chapters {
namespace {

std::string trim(std::string value) {
  const auto nonSpace = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), nonSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), nonSpace).base(),
              value.end());
  return value;
}

std::string singleLine(std::string value, std::size_t maximumBytes) {
  for (char &ch : value) {
    if (ch == '\r' || ch == '\n' || ch == '\t')
      ch = ' ';
  }
  value = trim(std::move(value));
  if (!isValidUtf8(value)) {
    value = wideToUtf8Lossy(utf8ToWideLossy(value));
  }
  if (value.size() > maximumBytes) {
    value.resize(maximumBytes);
    while (!value.empty() && !isValidUtf8(value))
      value.pop_back();
  }
  return value;
}

std::vector<InferenceDialogueCue>
englishDialogueFor(const AnalysisRequest &request) {
  std::vector<InferenceDialogueCue> dialogue;
  if (!request.englishText)
    return dialogue;

  dialogue.reserve(request.englishText->cues.size());
  for (const TextCue &cue : request.englishText->cues) {
    if (cue.startUs < 0 || cue.startUs >= request.durationUs)
      continue;
    std::string text = singleLine(cue.text, 600);
    if (!text.empty())
      dialogue.push_back({cue.startUs, std::move(text)});
  }
  std::stable_sort(
      dialogue.begin(), dialogue.end(),
      [](const InferenceDialogueCue &left, const InferenceDialogueCue &right) {
        return left.timeUs < right.timeUs;
      });
  return dialogue;
}

class DefaultBackend final : public Backend {
public:
  std::optional<AnalysisResult>
  cached(const AnalysisRequest &request) override {
    if (englishDialogueFor(request).empty())
      return std::nullopt;
    std::optional<AnalysisResult> result = loadCachedAnalysis(request);
    if (result) {
      discardInferenceWorkerWorkspaceIfIdle(analysisSourceKey(request));
      clearWorkspace();
    }
    return result;
  }

  CapabilityResult inspect(const AnalysisRequest &request,
                           const OperationControl &control) override {
    if (englishDialogueFor(request).empty()) {
      return {CapabilityState::Unsupported,
              "Automatic chapters require an English timecoded transcript "
              "or subtitle track."};
    }
    if (request.durationUs < kMinimumAutomaticChapterVideoDurationUs) {
      return {CapabilityState::Unsupported,
              "Automatic chapters require a video of at least 30 seconds."};
    }
    if (request.durationUs > kMaximumAutomaticChapterVideoDurationUs) {
      return {CapabilityState::Unsupported,
              "Automatic chapters currently support videos up to 60 "
              "minutes."};
    }
    paths_ = resolveModelPaths();
    const CapabilityResult gpu = inference_.inspect(control);
    if (gpu.state != CapabilityState::Ready)
      return gpu;
    std::string decodeDetail;
    const OperationStatus decode =
        probeHardwareVideoDecode(request, control, &decodeDetail);
    if (decode == OperationStatus::Yielded) {
      return {CapabilityState::Yielded, std::move(decodeDetail)};
    }
    if (decode == OperationStatus::Cancelled) {
      return {CapabilityState::Cancelled, {}};
    }
    if (decode != OperationStatus::Succeeded) {
      return {CapabilityState::Unsupported,
              decodeDetail.empty()
                  ? "D3D11 hardware video decoding is unavailable for this "
                    "video."
                  : std::move(decodeDetail)};
    }
    if (artifactsVerified_)
      return {CapabilityState::Ready, {}};
    CapabilityResult result = inspectModelArtifacts(paths_, control);
    if (result.state == CapabilityState::Ready)
      artifactsVerified_ = true;
    return result;
  }

  InstallResult install(const OperationControl &control) override {
    paths_ = resolveModelPaths();
    InstallResult result = installModelArtifacts(paths_, control);
    if (result.status == OperationStatus::Succeeded) {
      artifactsVerified_ = true;
    }
    return result;
  }

  AnalysisResult analyze(const AnalysisRequest &request,
                         const OperationControl &control) override {
    const std::vector<InferenceDialogueCue> englishDialogue =
        englishDialogueFor(request);
    if (englishDialogue.empty()) {
      return {OperationStatus::Unsupported,
              "Automatic chapters require an English timecoded transcript "
              "or subtitle track.",
              {},
              {}};
    }
    if (request.durationUs < kMinimumAutomaticChapterVideoDurationUs) {
      return {OperationStatus::Unsupported,
              "Automatic chapters require a video of at least 30 seconds.",
              {},
              {}};
    }
    if (request.durationUs > kMaximumAutomaticChapterVideoDurationUs) {
      return {OperationStatus::Unsupported,
              "Automatic chapters currently support videos up to 60 "
              "minutes.",
              {},
              {}};
    }
    if (std::optional<AnalysisResult> found = loadCachedAnalysis(request)) {
      discardInferenceWorkerWorkspaceIfIdle(analysisSourceKey(request));
      clearWorkspace();
      return *found;
    }
    const std::string sourceKey = analysisSourceKey(request);
    if (sourceKey.empty()) {
      clearWorkspace();
      return {OperationStatus::Failed,
               "Could not establish a stable chapter-analysis identity.",
               {},
               {}};
    }
    if (sourceKey != workspaceSourceKey_) {
      clearWorkspace();
      workspaceSourceKey_ = sourceKey;
    }
    if (paths_.model.empty())
      paths_ = resolveModelPaths();
    const CapabilityResult gpu = inference_.inspect(control);
    if (gpu.state != CapabilityState::Ready) {
      const OperationStatus status =
          gpu.state == CapabilityState::Yielded
              ? OperationStatus::Yielded
              : (gpu.state == CapabilityState::Cancelled
                     ? OperationStatus::Cancelled
                     : OperationStatus::Unsupported);
      if (status != OperationStatus::Yielded)
        clearWorkspace();
      return {status, gpu.detail, {}, {}};
    }

    InferenceWorkspaceLeaseResult acquired =
        acquireInferenceWorkspaceLease(sourceKey, control);
    if (acquired.status != OperationStatus::Succeeded) {
      if (acquired.status != OperationStatus::Yielded)
        clearWorkspace();
      return {acquired.status, std::move(acquired.detail), {}, {}};
    }
    // Cache publication and every resumable worker stage share one exact
    // source-level owner. A second process can neither repeat selection nor
    // consume a checkpoint while this transaction is active.
    if (std::optional<AnalysisResult> found = loadCachedAnalysis(request)) {
      discardInferenceWorkerWorkspace(acquired.lease);
      clearWorkspace();
      return *found;
    }

    if (chapterPlan_.empty()) {
      SpeechChapterPlanRequest planRequest;
      planRequest.plannerModel = paths_.plannerModel;
      planRequest.planAdapter = paths_.speechPlanAdapter;
      planRequest.durationUs = request.durationUs;
      planRequest.englishDialogue = englishDialogue;
      OperationControl planControl = control;
      planControl.progress = [publish = control.progress](
                                      std::optional<double> progress,
                                      const std::string &phase) {
        if (!publish)
          return;
        publish(progress ? std::optional<double>(
                               0.02 + 0.50 * std::clamp(*progress, 0.0, 1.0))
                         : std::nullopt,
                phase);
      };
      const SpeechChapterPlanResult plan =
          runSpeechChapterPlanWorker(planRequest, planControl, acquired.lease);
      if (plan.status != OperationStatus::Succeeded) {
        if (plan.status != OperationStatus::Yielded)
          clearWorkspace();
        return {plan.status, plan.detail, {}, {}};
      }
      chapterPlan_ = plan.chapterPlan;
      std::vector<std::int64_t> chapterStartsUs;
      chapterStartsUs.reserve(chapterPlan_.size());
      for (const GeneratedChapterPlanEntry &chapter : chapterPlan_)
        chapterStartsUs.push_back(chapter.startUs);
      evidencePlan_ = buildSpeechGuidedChapterEvidencePlan(
          request.durationUs, chapterStartsUs);
      if (evidencePlan_.empty()) {
        clearWorkspace();
        return {OperationStatus::Failed,
                "The speech-guided chapter planner returned an invalid "
                "frame plan.",
                {},
                {}};
      }
    }
    if (evidenceCheckpoint_.intervals.empty() ||
        evidenceCheckpoint_.windows.size() !=
            evidenceCheckpoint_.intervals.size()) {
      if (control.progress)
        control.progress(0.52, "Preparing video samples");
      OperationControl evidenceControl = control;
      evidenceControl.progress = [publish = control.progress](
                                     std::optional<double> progress,
                                     const std::string &phase) {
        if (!publish)
          return;
        publish(progress ? std::optional<double>(
                               0.52 + 0.04 * std::clamp(*progress, 0.0, 1.0))
                         : std::nullopt,
                phase);
      };
      SampledEvidenceResult evidence = sampleVideoEvidence(
          request, evidenceControl, &evidenceCheckpoint_, evidencePlan_);
      if (evidence.status != OperationStatus::Succeeded) {
        if (evidence.status != OperationStatus::Yielded)
          clearWorkspace();
        return {evidence.status, std::move(evidence.detail), {}, {}};
      }
    }
    if (control.progress) {
      control.progress(0.56, "Preparing isolated Vulkan inference");
    }

    InferenceRequest inferenceRequest;
    inferenceRequest.model = paths_.model;
    inferenceRequest.projector = paths_.projector;
    inferenceRequest.plannerModel = paths_.plannerModel;
    inferenceRequest.chapterPlanAdapter = paths_.chapterPlanAdapter;
    inferenceRequest.durationUs = request.durationUs;
    inferenceRequest.windows.reserve(evidenceCheckpoint_.windows.size());
    for (const SampledTemporalWindow &sampledWindow :
         evidenceCheckpoint_.windows) {
      InferenceTemporalWindow window;
      window.intervalStartUs = sampledWindow.startUs;
      window.intervalEndUs = sampledWindow.endUs;
      window.frames.reserve(sampledWindow.frames.size());
      for (const SampledFrame &frame : sampledWindow.frames) {
        window.frames.push_back(
            {frame.width, frame.height, &frame.rgb, frame.timeUs});
      }
      inferenceRequest.windows.push_back(std::move(window));
    }
    inferenceRequest.englishDialogue = englishDialogue;
    inferenceRequest.chapterPlan = chapterPlan_;
    std::string budgetError;
    if (!validateInferenceInputBudget(inferenceRequest, &budgetError)) {
      clearWorkspace();
      return {OperationStatus::Unsupported, std::move(budgetError), {}, {}};
    }
    const InferenceResult inference =
        runInferenceWorker(inferenceRequest, control, acquired.lease);
    if (inference.status != OperationStatus::Succeeded) {
      if (inference.status != OperationStatus::Yielded)
        clearWorkspace();
      return {inference.status, inference.detail, {}, {}};
    }
    if (control.progress)
      control.progress(0.996, "Validating chapter output");
    AnalysisResult result =
        materializeGeneratedDocument(inference.document, request.durationUs);
    if (result.status != OperationStatus::Succeeded) {
      // A structurally complete but unpublishable artifact must not become a
      // permanent retry loop. Resume checkpoints are valuable only while the
      // staged document remains capable of passing the public contract.
      discardInferenceWorkerWorkspace(acquired.lease);
      clearWorkspace();
      return result;
    }
    std::string cacheError;
    if (!storeCachedAnalysis(request, result, &cacheError)) {
      result.warning =
          "Chapter analysis is available for this session, but it could "
          "not be saved for reuse";
      if (!cacheError.empty())
        result.warning += ": " + cacheError;
    } else {
      discardInferenceWorkerWorkspace(acquired.lease);
    }
    if (control.progress)
      control.progress(1.0, "Chapter analysis complete");
    clearWorkspace();
    return result;
  }

private:
  void clearWorkspace() {
    workspaceSourceKey_.clear();
    evidenceCheckpoint_ = {};
    evidencePlan_.clear();
    chapterPlan_.clear();
  }

  ModelPaths paths_;
  InferenceEngine inference_;
  std::string workspaceSourceKey_;
  SampledEvidenceCheckpoint evidenceCheckpoint_;
  std::vector<ChapterEvidenceInterval> evidencePlan_;
  std::vector<GeneratedChapterPlanEntry> chapterPlan_;
  bool artifactsVerified_ = false;
};

} // namespace

std::unique_ptr<Backend> createDefaultBackend() {
  return std::make_unique<DefaultBackend>();
}

} // namespace playback_video_chapters
