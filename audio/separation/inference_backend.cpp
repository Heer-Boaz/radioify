#include "audio/separation/inference_backend.h"

namespace audio_separation {

bool InferenceBackend::valid() const {
  return !providerName.empty() && !displayName.empty() &&
         !diagnosticComponent.empty();
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
  backend.providerOptions.push_back({"enable_graph_capture", "true"});
  return backend;
}

}  // namespace audio_separation
