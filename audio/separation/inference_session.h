#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "audio/separation/diagnostics.h"
#include "audio/separation/execution_control.h"
#include "audio/separation/inference_backend.h"

namespace audio_separation {

// Owns one ONNX Runtime environment and its selected GPU device. Model loading
// and ahead-of-time compilation use the same session contract so provider
// registration, device selection, dimensions, and provider options cannot
// drift apart.
class InferenceSessionFactory {
 public:
  static std::unique_ptr<InferenceSessionFactory> create(
      InferenceBackend backend, DiagnosticReporter diagnostics,
      std::string* error);

  ~InferenceSessionFactory();

  InferenceSessionFactory(const InferenceSessionFactory&) = delete;
  InferenceSessionFactory& operator=(const InferenceSessionFactory&) = delete;

  std::unique_ptr<Ort::Session> loadModel(
      const std::filesystem::path& modelPath,
      const ExecutionControl* control = nullptr);
  bool compileModel(const std::filesystem::path& sourceModelPath,
                    const std::filesystem::path& compiledModelPath,
                    const ExecutionControl& control,
                    std::string* error);

  const InferenceBackend& backend() const { return backend_; }
  const DiagnosticReporter& diagnostics() const { return diagnostics_; }

 private:
  InferenceSessionFactory(InferenceBackend backend,
                          DiagnosticReporter diagnostics);

  bool initialize(std::string* error);
  Ort::SessionOptions makeSessionOptions();

  InferenceBackend backend_;
  DiagnosticReporter diagnostics_;
  Ort::Env environment_;
  std::vector<Ort::ConstEpDevice> devices_;
};

}  // namespace audio_separation
