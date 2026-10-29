#include "audio/separation/model.h"

#include "core/model_download.h"
#include "core/runtime_helpers.h"

namespace audio_separation {

bool ensureSeparationModel(std::filesystem::path *modelPath,
                           const ProgressCallback &onProgress,
                           const ExecutionControl &control,
                           std::string *error) {
  constexpr model_download::Artifact artifact{
      L"media.githubusercontent.com",
      L"/media/Heer-Boaz/radioify/"
      L"fbb3aa69bbf8ef32cd1c404b61e30102b7a177e5/"
      L"models/audio_separation/bandit-v2-multi-mask-core-fp16.onnx",
      "Downloading audio separation model (76 MB)", 75781693ull,
      RADIOIFY_AUDIO_SEPARATION_MODEL_SHA256};
  *modelPath = radioifyWritableDataDir() / "models" / "audio_separation" /
               "bandit-v2-multi-mask-core-fp16.onnx";
  model_download::Control downloadControl;
  downloadControl.cancelled = [&] { return !control.checkpoint(); };
  downloadControl.progress = [&](double fraction, const std::string &phase) {
    if (onProgress)
      onProgress({static_cast<float>(fraction), phase});
  };
  const auto result =
      model_download::ensure(*modelPath, artifact, downloadControl);
  if (result.status == model_download::Status::Succeeded)
    return true;
  if (error)
    *error = result.detail;
  modelPath->clear();
  return false;
}

} // namespace audio_separation
