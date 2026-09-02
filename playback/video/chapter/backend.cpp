#include "playback/video/chapter/backend.h"

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

#include "core/utf8.h"
#include "playback/video/chapter/cache.h"
#include "playback/video/chapter/generated_document.h"
#include "playback/video/chapter/inference.h"
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
  for (char& ch : value) {
    if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
  }
  value = trim(std::move(value));
  if (!isValidUtf8(value)) {
    value = wideToUtf8Lossy(utf8ToWideLossy(value));
  }
  if (value.size() > maximumBytes) {
    value.resize(maximumBytes);
    while (!value.empty() && !isValidUtf8(value)) value.pop_back();
  }
  return value;
}

std::string dialogueForSample(const AnalysisRequest& request,
                              const std::vector<SampledFrame>& frames,
                              std::size_t index) {
  if (!request.englishText || index >= frames.size()) return {};
  const std::int64_t currentUs = frames[index].timeUs;
  const std::int64_t beginUs =
      index == 0 ? 0
                 : frames[index - 1].timeUs +
                       (currentUs - frames[index - 1].timeUs) / 2;
  const std::int64_t endUs =
      index + 1 == frames.size()
          ? request.durationUs
          : currentUs + (frames[index + 1].timeUs - currentUs) / 2;
  return singleLine(textInInterval(*request.englishText, beginUs, endUs, 600),
                    600);
}

class DefaultBackend final : public Backend {
 public:
  std::optional<AnalysisResult> cached(
      const AnalysisRequest& request) override {
    std::optional<AnalysisResult> result = loadCachedAnalysis(request);
    if (result) clearWorkspace();
    return result;
  }

  CapabilityResult inspect(const AnalysisRequest& request,
                           const OperationControl& control) override {
    if (request.durationUs < kMinimumAutomaticChapterVideoDurationUs) {
      return {CapabilityState::Unsupported,
              "Automatic chapters require a video of at least 30 seconds."};
    }
    paths_ = resolveModelPaths();
    const CapabilityResult gpu = inference_.inspect(control);
    if (gpu.state != CapabilityState::Ready) return gpu;
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
    if (artifactsVerified_) return {CapabilityState::Ready, {}};
    CapabilityResult result = inspectModelArtifacts(paths_, control);
    if (result.state == CapabilityState::Ready) artifactsVerified_ = true;
    return result;
  }

  InstallResult install(const OperationControl& control) override {
    paths_ = resolveModelPaths();
    InstallResult result = installModelArtifacts(paths_, control);
    if (result.status == OperationStatus::Succeeded) {
      artifactsVerified_ = true;
    }
    return result;
  }

  AnalysisResult analyze(const AnalysisRequest& request,
                         const OperationControl& control) override {
    if (request.durationUs < kMinimumAutomaticChapterVideoDurationUs) {
      return {OperationStatus::Unsupported,
              "Automatic chapters require a video of at least 30 seconds.",
              {},
              {}};
    }
    if (std::optional<AnalysisResult> found = loadCachedAnalysis(request)) {
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
    if (paths_.model.empty()) paths_ = resolveModelPaths();
    const CapabilityResult gpu = inference_.inspect(control);
    if (gpu.state != CapabilityState::Ready) {
      const OperationStatus status =
          gpu.state == CapabilityState::Yielded
              ? OperationStatus::Yielded
              : (gpu.state == CapabilityState::Cancelled
                     ? OperationStatus::Cancelled
                     : OperationStatus::Unsupported);
      if (status != OperationStatus::Yielded) clearWorkspace();
      return {status, gpu.detail, {}, {}};
    }
    if (workspaceFrames_.empty()) {
      if (control.progress) control.progress(0.0, "Preparing video samples");
      SampledEvidenceResult evidence = sampleVideoEvidence(request, control);
      if (evidence.status != OperationStatus::Succeeded) {
        if (evidence.status != OperationStatus::Yielded) clearWorkspace();
        return {evidence.status, std::move(evidence.detail), {}, {}};
      }
      workspaceFrames_ = std::move(evidence.frames);
    }
    if (control.progress) {
      control.progress(0.58, "Loading Qwen2.5-VL on Vulkan");
    }

    InferenceRequest inferenceRequest;
    inferenceRequest.model = paths_.model;
    inferenceRequest.projector = paths_.projector;
    inferenceRequest.durationUs = request.durationUs;
    inferenceRequest.images.reserve(workspaceFrames_.size());
    std::vector<std::int64_t> sampleTimesUs;
    sampleTimesUs.reserve(workspaceFrames_.size());
    for (std::size_t index = 0; index < workspaceFrames_.size(); ++index) {
      const SampledFrame& frame = workspaceFrames_[index];
      inferenceRequest.images.push_back(
          {frame.width, frame.height, &frame.rgb, frame.timeUs,
           dialogueForSample(request, workspaceFrames_, index)});
      sampleTimesUs.push_back(frame.timeUs);
    }
    const InferenceResult inference =
        inference_.run(inferenceRequest, control, &inferenceCheckpoint_);
    if (inference.status != OperationStatus::Succeeded) {
      if (inference.status != OperationStatus::Yielded) clearWorkspace();
      return {inference.status, inference.detail, {}, {}};
    }
    if (control.progress) control.progress(0.98, "Validating chapter output");
    AnalysisResult result = materializeGeneratedDocument(
        inference.document, request.durationUs, sampleTimesUs);
    if (result.status != OperationStatus::Succeeded) {
      clearWorkspace();
      return result;
    }
    std::string cacheError;
    if (!storeCachedAnalysis(request, result, &cacheError)) {
      result.status = OperationStatus::Failed;
      result.detail = cacheError.empty()
                          ? "Could not publish the chapter analysis cache."
                          : std::move(cacheError);
      result.overview.clear();
      result.chapters.clear();
      clearWorkspace();
      return result;
    }
    if (control.progress) control.progress(1.0, "Chapter analysis complete");
    clearWorkspace();
    return result;
  }

 private:
  void clearWorkspace() {
    workspaceSourceKey_.clear();
    workspaceFrames_.clear();
    inferenceCheckpoint_ = {};
  }

  ModelPaths paths_;
  InferenceEngine inference_;
  std::string workspaceSourceKey_;
  std::vector<SampledFrame> workspaceFrames_;
  InferenceCheckpoint inferenceCheckpoint_;
  bool artifactsVerified_ = false;
};

}  // namespace

std::unique_ptr<Backend> createDefaultBackend() {
  return std::make_unique<DefaultBackend>();
}

}  // namespace playback_video_chapters
