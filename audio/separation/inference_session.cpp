#include "audio/separation/inference_session.h"

#include <cstdint>
#include <sstream>
#include <utility>

#include <onnxruntime_session_options_config_keys.h>

#include "audio/separation/mask_model.h"
#include "audio/separation/spectral_transform.h"

namespace audio_separation {
namespace {

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

std::string pathDescription(const std::filesystem::path& path) {
  const std::u8string value = path.u8string();
  return std::string(reinterpret_cast<const char*>(value.data()),
                     value.size());
}

DiagnosticLevel diagnosticLevel(OrtLoggingLevel level) {
  switch (level) {
    case ORT_LOGGING_LEVEL_WARNING:
      return DiagnosticLevel::Warning;
    case ORT_LOGGING_LEVEL_ERROR:
    case ORT_LOGGING_LEVEL_FATAL:
      return DiagnosticLevel::Error;
    case ORT_LOGGING_LEVEL_VERBOSE:
    case ORT_LOGGING_LEVEL_INFO:
      return DiagnosticLevel::Info;
  }
  return DiagnosticLevel::Info;
}

void ORT_API_CALL onnxRuntimeLog(void* parameter, OrtLoggingLevel severity,
                                 const char* category, const char*,
                                 const char* codeLocation,
                                 const char* message) {
  const auto* reporter = static_cast<const DiagnosticReporter*>(parameter);
  if (!reporter || !*reporter) return;
  try {
    std::string detail = message ? message : "(empty ONNX Runtime message)";
    if (codeLocation && codeLocation[0] != '\0') {
      detail += " [";
      detail += codeLocation;
      detail += ']';
    }
    (*reporter)(diagnosticLevel(severity),
                category && category[0] != '\0' ? category : "onnxruntime",
                detail);
  } catch (...) {
    // A diagnostics sink must never cross the C callback boundary.
  }
}

}  // namespace

void enforceGpuOnlyExecution(Ort::SessionOptions& options) {
  options.AddConfigEntry(kOrtSessionOptionsDisableCPUEPFallback, "1");
}

InferenceSessionFactory::InferenceSessionFactory(
    InferenceBackend backend, DiagnosticReporter diagnostics)
    : backend_(std::move(backend)),
      diagnostics_(std::move(diagnostics)),
      environment_(ORT_LOGGING_LEVEL_WARNING, "radioify-separation",
                   onnxRuntimeLog, &diagnostics_) {}

InferenceSessionFactory::~InferenceSessionFactory() = default;

std::unique_ptr<InferenceSessionFactory> InferenceSessionFactory::create(
    InferenceBackend backend, DiagnosticReporter diagnostics,
    std::string* error) {
  if (error) error->clear();
  if (!backend.valid()) {
    setError(error, "The audio-separation inference backend is invalid.");
    return nullptr;
  }
  try {
    auto factory = std::unique_ptr<InferenceSessionFactory>(
        new InferenceSessionFactory(std::move(backend),
                                    std::move(diagnostics)));
    if (!factory->initialize(error)) return nullptr;
    return factory;
  } catch (const Ort::Exception& exception) {
    setError(error, "Could not initialize the audio-separation runtime: " +
                        std::string(exception.what()));
    return nullptr;
  } catch (const std::exception& exception) {
    setError(error, "Could not initialize the audio-separation runtime: " +
                        std::string(exception.what()));
    return nullptr;
  }
}

bool InferenceSessionFactory::initialize(std::string* error) {
  reportDiagnostic(diagnostics_, DiagnosticLevel::Info, "onnxruntime",
                   "Version: " + std::string(Ort::GetVersionString()));
  if (!backend_.providerLibrary.empty()) {
    environment_.RegisterExecutionProviderLibrary(
        backend_.providerName.c_str(), backend_.providerLibrary.native());
    reportDiagnostic(diagnostics_, DiagnosticLevel::Info,
                     backend_.diagnosticComponent,
                     "Registered provider library: " +
                         pathDescription(backend_.providerLibrary));
  }

  for (const Ort::ConstEpDevice& device : environment_.GetEpDevices()) {
    if (std::string(device.EpName()) == backend_.providerName &&
        device.Device().Type() == OrtHardwareDeviceType_GPU) {
      devices_.push_back(device);
    }
  }
  if (devices_.empty()) {
    setError(error, "No " + backend_.displayName +
                        " GPU is available; CPU audio separation is "
                        "disabled.");
    return false;
  }

  const Ort::ConstHardwareDevice hardware = devices_.front().Device();
  std::ostringstream description;
  description << "Selected " << backend_.displayName
              << " GPU; vendor="
              << (hardware.Vendor() ? hardware.Vendor() : "unknown")
              << ", vendor_id=0x" << std::hex << hardware.VendorId()
              << ", device_id=0x" << hardware.DeviceId();
  reportDiagnostic(diagnostics_, DiagnosticLevel::Info,
                   backend_.diagnosticComponent, description.str());
  return true;
}

Ort::SessionOptions InferenceSessionFactory::makeSessionOptions() {
  Ort::SessionOptions options;
  options.DisableMemPattern();
  options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  enforceGpuOnlyExecution(options);
  options.AddFreeDimensionOverrideByName(
      "batch", static_cast<std::int64_t>(BanditMaskModel::kBatchSize));
  options.AddFreeDimensionOverrideByName(
      "time", BanditSpectralTransform::kTimeFrames);
  Ort::KeyValuePairs providerOptions;
  for (const InferenceProviderOption& option : backend_.providerOptions) {
    providerOptions.Add(option.key.c_str(), option.value.c_str());
  }
  options.AppendExecutionProvider_V2(environment_, devices_,
                                     providerOptions);
  return options;
}

std::unique_ptr<Ort::Session> InferenceSessionFactory::loadModel(
    const std::filesystem::path& modelPath,
    const ExecutionControl* control) {
  Ort::SessionOptions options = makeSessionOptions();
  ExecutionControl::InterruptionRegistration interruption;
  if (control) {
    if (control->cancellationRequested()) {
      options.SetLoadCancellationFlag(true);
    }
    interruption = control->registerInterruption(
        [&options]() { options.SetLoadCancellationFlag(true); });
  }
  return std::make_unique<Ort::Session>(environment_, modelPath.c_str(),
                                        options);
}

bool InferenceSessionFactory::compileModel(
    const std::filesystem::path& sourceModelPath,
    const std::filesystem::path& compiledModelPath,
    const ExecutionControl& control, std::string* error) {
  if (error) error->clear();
  try {
    Ort::SessionOptions sessionOptions = makeSessionOptions();
    if (control.cancellationRequested()) {
      sessionOptions.SetLoadCancellationFlag(true);
    }
    auto interruption = control.registerInterruption(
        [&sessionOptions]() {
          sessionOptions.SetLoadCancellationFlag(true);
        });
    Ort::ModelCompilationOptions compilationOptions(environment_,
                                                    sessionOptions);
    compilationOptions.SetInputModelPath(sourceModelPath.c_str());
    compilationOptions.SetOutputModelPath(compiledModelPath.c_str());
    compilationOptions.SetEpContextEmbedMode(true);
    compilationOptions.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_ALL);
    const Ort::Status status = Ort::CompileModel(environment_,
                                                 compilationOptions);
    if (!status.IsOK()) {
      setError(error, status.GetErrorMessage());
      return false;
    }
    return true;
  } catch (const Ort::Exception& exception) {
    setError(error, exception.what());
    return false;
  } catch (const std::exception& exception) {
    setError(error, exception.what());
    return false;
  }
}

}  // namespace audio_separation
