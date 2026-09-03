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

class DefaultBackend final : public Backend {
public:
  std::optional<AnalysisResult>
  cached(const AnalysisRequest &request) override {
    std::optional<AnalysisResult> result = loadCachedAnalysis(request);
    if (result) {
      discardInferenceWorkerWorkspaceIfIdle(analysisSourceKey(request));
      clearWorkspace();
    }
    return result;
  }

  CapabilityResult inspect(const AnalysisRequest &request,
                           const OperationControl &control) override {
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
    if (request.durationUs < kMinimumAutomaticChapterVideoDurationUs) {
      return {OperationStatus::Unsupported,
              "Automatic chapters require a video of at least 30 seconds.",
              {},
              {},
              {}};
    }
    if (request.durationUs > kMaximumAutomaticChapterVideoDurationUs) {
      return {OperationStatus::Unsupported,
              "Automatic chapters currently support videos up to 60 "
              "minutes.",
              {},
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
      return {status, gpu.detail, {}, {}, {}};
    }
    if (evidenceCheckpoint_.intervals.empty() ||
        evidenceCheckpoint_.windows.size() !=
            evidenceCheckpoint_.intervals.size()) {
      if (control.progress)
        control.progress(0.0, "Preparing video samples");
      SampledEvidenceResult evidence =
          sampleVideoEvidence(request, control, &evidenceCheckpoint_);
      if (evidence.status != OperationStatus::Succeeded) {
        if (evidence.status != OperationStatus::Yielded)
          clearWorkspace();
        return {evidence.status, std::move(evidence.detail), {}, {}, {}};
      }
    }
    if (control.progress) {
      control.progress(0.56, "Preparing isolated Vulkan inference");
    }

    InferenceRequest inferenceRequest;
    inferenceRequest.model = paths_.model;
    inferenceRequest.projector = paths_.projector;
    inferenceRequest.plannerModel = paths_.plannerModel;
    inferenceRequest.plannerAdapter = paths_.plannerAdapter;
    inferenceRequest.durationUs = request.durationUs;
    inferenceRequest.windows.reserve(evidenceCheckpoint_.windows.size());
    std::vector<std::int64_t> boundaryAnchorsUs;
    boundaryAnchorsUs.reserve(evidenceCheckpoint_.windows.size() * 6u + 1u);
    boundaryAnchorsUs.push_back(0);
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
      for (const SampledFrame &frame : sampledWindow.frames) {
        if (frame.timeUs > 0 && frame.timeUs != boundaryAnchorsUs.back())
          boundaryAnchorsUs.push_back(frame.timeUs);
      }
    }
    if (request.englishText) {
      inferenceRequest.englishDialogue.reserve(
          request.englishText->cues.size());
      for (const TextCue &cue : request.englishText->cues) {
        if (cue.startUs < 0 || cue.startUs >= request.durationUs)
          continue;
        std::string text = singleLine(cue.text, 600);
        if (!text.empty())
          inferenceRequest.englishDialogue.push_back(
              {cue.startUs, std::move(text)});
      }
      std::stable_sort(inferenceRequest.englishDialogue.begin(),
                       inferenceRequest.englishDialogue.end(),
                       [](const InferenceDialogueCue &left,
                          const InferenceDialogueCue &right) {
                         return left.timeUs < right.timeUs;
                       });
    }
    std::string budgetError;
    if (!validateInferenceInputBudget(inferenceRequest, &budgetError)) {
      clearWorkspace();
      return {OperationStatus::Unsupported, std::move(budgetError), {}, {}, {}};
    }
    InferenceWorkspaceLeaseResult acquired =
        acquireInferenceWorkspaceLease(sourceKey, control);
    if (acquired.status != OperationStatus::Succeeded) {
      if (acquired.status != OperationStatus::Yielded)
        clearWorkspace();
      return {acquired.status, std::move(acquired.detail), {}, {}, {}};
    }
    // Another process may have completed this exact source while this process
    // was sampling or waiting for the workspace lease. Recheck only after
    // acquiring ownership so cache publication and checkpoint cleanup form one
    // atomic source-level transaction.
    if (std::optional<AnalysisResult> found = loadCachedAnalysis(request)) {
      discardInferenceWorkerWorkspace(acquired.lease);
      clearWorkspace();
      return *found;
    }
    const InferenceResult inference =
        runInferenceWorker(inferenceRequest, control, acquired.lease);
    if (inference.status != OperationStatus::Succeeded) {
      if (inference.status != OperationStatus::Yielded)
        clearWorkspace();
      return {inference.status, inference.detail, {}, {}, {}};
    }
    if (control.progress)
      control.progress(0.996, "Validating chapter output");
    AnalysisResult result = materializeGeneratedDocument(
        inference.document, request.durationUs, boundaryAnchorsUs);
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
  }

  ModelPaths paths_;
  InferenceEngine inference_;
  std::string workspaceSourceKey_;
  SampledEvidenceCheckpoint evidenceCheckpoint_;
  bool artifactsVerified_ = false;
};

} // namespace

std::unique_ptr<Backend> createDefaultBackend() {
  return std::make_unique<DefaultBackend>();
}

} // namespace playback_video_chapters
