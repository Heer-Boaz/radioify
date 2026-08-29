#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace audio_separation {

enum class InferenceBackendKind {
  DirectMl,
  WindowsMlNvidiaTensorRtRtx,
};

struct InferenceProviderOption {
  std::string key;
  std::string value;
};

// Describes one in-process ONNX Runtime execution provider. Discovery and
// product policy live outside the model adapter so that loading a model never
// installs software or silently chooses a different backend.
struct InferenceBackend {
  InferenceBackendKind kind = InferenceBackendKind::DirectMl;
  std::string providerName;
  std::string displayName;
  std::string diagnosticComponent;
  std::filesystem::path providerLibrary;
  std::vector<InferenceProviderOption> providerOptions;

  bool valid() const;
};

InferenceBackend directMlInferenceBackend();

}  // namespace audio_separation
