#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace audio_separation {

enum class InferenceBackendKind {
  Unspecified,
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
  // A default-constructed descriptor is deliberately unusable. Diagnostics
  // and production policy must both name their execution provider explicitly.
  InferenceBackendKind kind = InferenceBackendKind::Unspecified;
  std::string providerName;
  std::string displayName;
  std::string diagnosticComponent;
  std::string providerVersion;
  std::filesystem::path providerLibrary;
  std::vector<InferenceProviderOption> providerOptions;

  bool valid() const;
  void setProviderOption(std::string key, std::string value);
};

InferenceBackend directMlInferenceBackend();

}  // namespace audio_separation
