#include "playback/video/analysis/model.h"

#include "core/model_download.h"
#include "core/runtime_helpers.h"

namespace playback_video_analysis {

VideoReviewModelPaths resolveVideoReviewModelPaths() {
  const auto directory =
      (radioifyWritableDataDir() / "models") / "qwen3-vl-8b-q4-k-m";
  return {directory / "Qwen3VL-8B-Instruct-Q4_K_M.gguf",
          directory / "mmproj-Qwen3VL-8B-Instruct-F16.gguf"};
}

InstallResult installVideoReviewModel(const VideoReviewModelPaths &paths,
                                      const OperationControl &control) {
  constexpr model_download::Artifact model{
      L"huggingface.co",
      L"/Qwen/Qwen3-VL-8B-Instruct-GGUF/resolve/"
      L"f982a07559d4a2f6c8744d840bf6fccab30eea96/"
      L"Qwen3VL-8B-Instruct-Q4_K_M.gguf?download=true",
      "Downloading video editing model", 5027784800ull,
      kVideoReviewModelSha256};
  constexpr model_download::Artifact projector{
      L"huggingface.co",
      L"/Qwen/Qwen3-VL-8B-Instruct-GGUF/resolve/"
      L"f982a07559d4a2f6c8744d840bf6fccab30eea96/"
      L"mmproj-Qwen3VL-8B-Instruct-F16.gguf?download=true",
      "Downloading video editing projector", 1159029824ull,
      kVideoReviewProjectorSha256};
  const auto install = [&](const std::filesystem::path &path,
                           const model_download::Artifact &artifact,
                           std::uintmax_t completed) {
    model_download::Control downloadControl;
    downloadControl.cancelled = control.cancelled;
    downloadControl.progress = [&](double fraction, const std::string &phase) {
      if (control.progress)
        control.progress((completed + fraction * artifact.bytes) /
                             static_cast<double>(model.bytes + projector.bytes),
                         phase);
    };
    const auto result = model_download::ensure(path, artifact, downloadControl);
    const auto status = result.status == model_download::Status::Succeeded
                            ? OperationStatus::Succeeded
                        : result.status == model_download::Status::Cancelled
                            ? OperationStatus::Cancelled
                            : OperationStatus::Failed;
    return InstallResult{status, result.detail};
  };
  const auto result = install(paths.model, model, 0);
  if (result.status != OperationStatus::Succeeded)
    return result;
  return install(paths.projector, projector, model.bytes);
}

} // namespace playback_video_analysis
