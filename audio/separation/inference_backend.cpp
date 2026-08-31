#include "audio/separation/inference_backend.h"

#include <utility>

namespace audio_separation {

bool InferenceBackend::valid() const {
  return kind != InferenceBackendKind::Unspecified &&
         !providerName.empty() && !displayName.empty() &&
         !diagnosticComponent.empty();
}

void InferenceBackend::setProviderOption(std::string key,
                                         std::string value) {
  for (InferenceProviderOption& option : providerOptions) {
    if (option.key == key) {
      option.value = std::move(value);
      return;
    }
  }
  providerOptions.push_back({std::move(key), std::move(value)});
}

InferenceBackend directMlInferenceBackend() {
  InferenceBackend backend;
  backend.kind = InferenceBackendKind::DirectMl;
  backend.providerName = "DmlExecutionProvider";
  backend.displayName = "DirectML";
  backend.diagnosticComponent = "directml";
  // Fixed dimensions and stable reusable tensor addresses satisfy DirectML's
  // capture contract and avoid rebuilding the recurrent command graph for
  // every inference window.
  backend.setProviderOption("enable_graph_capture", "true");
  return backend;
}

}  // namespace audio_separation
