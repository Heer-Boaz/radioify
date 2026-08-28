#include "audio/separation/mask_model.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <sstream>
#include <utility>

#include "audio/separation/artifact.h"
#include "audio/separation/spectral_transform.h"

namespace audio_separation {
namespace {

constexpr const char* kInputName = "spectrogram_ri";
constexpr const char* kOutputName = "masks_ri";
constexpr std::array<std::int64_t, 5> kInputShape = {
    static_cast<std::int64_t>(BanditMaskModel::kBatchSize),
    1,
    static_cast<std::int64_t>(BanditSpectralTransform::kFrequencyBins),
    static_cast<std::int64_t>(BanditSpectralTransform::kTimeFrames),
    2};
constexpr std::array<std::int64_t, 6> kOutputShape = {
    static_cast<std::int64_t>(BanditMaskModel::kBatchSize),
    static_cast<std::int64_t>(kStemCount),
    1,
    static_cast<std::int64_t>(BanditSpectralTransform::kFrequencyBins),
    static_cast<std::int64_t>(BanditSpectralTransform::kTimeFrames),
    2};
constexpr std::size_t kOutputValues =
    BanditMaskModel::kBatchSize * kStemCount *
    BanditSpectralTransform::kRealImagValues;
constexpr std::size_t kInputValues =
    BanditMaskModel::kBatchSize *
    BanditSpectralTransform::kRealImagValues;

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

bool cancelled(const std::atomic<bool>* cancelRequested) {
  return cancelRequested &&
         cancelRequested->load(std::memory_order_relaxed);
}

template <std::size_t Size>
bool matchesShape(const std::vector<std::int64_t>& actual,
                  const std::array<std::int64_t, Size>& expected) {
  return actual.size() == expected.size() &&
         std::equal(actual.begin(), actual.end(), expected.begin());
}

std::string shapeDescription(const std::vector<std::int64_t>& dimensions) {
  std::ostringstream description;
  description << '[';
  for (std::size_t index = 0; index < dimensions.size(); ++index) {
    if (index > 0) description << ',';
    description << dimensions[index];
  }
  description << ']';
  return description.str();
}

std::string symbolicShapeDescription(
    const std::vector<const char*>& dimensions) {
  std::ostringstream description;
  description << '[';
  for (std::size_t index = 0; index < dimensions.size(); ++index) {
    if (index > 0) description << ',';
    const char* name = dimensions[index];
    description << (name && name[0] != '\0' ? name : "-");
  }
  description << ']';
  return description.str();
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
  const auto* reporter =
      static_cast<const DiagnosticReporter*>(parameter);
  if (!reporter || !*reporter) return;
  try {
    std::string detail = message ? message : "(empty ONNX Runtime message)";
    if (codeLocation && codeLocation[0] != '\0') {
      detail += " [";
      detail += codeLocation;
      detail += "]";
    }
    (*reporter)(diagnosticLevel(severity),
                category && category[0] != '\0' ? category : "onnxruntime",
                detail);
  } catch (...) {
    // A diagnostics sink must never cross the C callback boundary.
  }
}

std::string elementTypeName(ONNXTensorElementDataType type) {
  switch (type) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
      return "float32";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
      return "float16";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:
      return "float64";
    default:
      return "ONNX tensor type " + std::to_string(static_cast<int>(type));
  }
}

bool matchesModelContract(Ort::Session& session,
                          const DiagnosticReporter& diagnostics,
                          std::string* error) {
  if (session.GetInputCount() != 1 || session.GetOutputCount() != 1) {
    setError(error, "The audio-separation model has an unexpected graph interface.");
    return false;
  }
  Ort::AllocatorWithDefaultOptions allocator;
  const auto inputName = session.GetInputNameAllocated(0, allocator);
  const auto outputName = session.GetOutputNameAllocated(0, allocator);
  if (!inputName || !outputName ||
      std::strcmp(inputName.get(), kInputName) != 0 ||
      std::strcmp(outputName.get(), kOutputName) != 0) {
    setError(error, "The audio-separation model has unexpected tensor names.");
    return false;
  }
  // Tensor type-and-shape views are borrowed from their owning TypeInfo.
  // Keep both owners alive for every metadata read below.
  const Ort::TypeInfo inputTypeInfo = session.GetInputTypeInfo(0);
  const Ort::TypeInfo outputTypeInfo = session.GetOutputTypeInfo(0);
  const auto inputInfo = inputTypeInfo.GetTensorTypeAndShapeInfo();
  const auto outputInfo = outputTypeInfo.GetTensorTypeAndShapeInfo();
  const ONNXTensorElementDataType inputType = inputInfo.GetElementType();
  const ONNXTensorElementDataType outputType = outputInfo.GetElementType();
  if (inputType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
      outputType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
    setError(error, "Graph input is " + elementTypeName(inputType) +
                        " and output is " + elementTypeName(outputType) +
                        "; Radioify requires float32 for both.");
    return false;
  }
  const std::vector<std::int64_t> inputShape = inputInfo.GetShape();
  const std::vector<std::int64_t> outputShape = outputInfo.GetShape();
  const std::vector<const char*> inputSymbols =
      inputInfo.GetSymbolicDimensions();
  const std::vector<const char*> outputSymbols =
      outputInfo.GetSymbolicDimensions();
  reportDiagnostic(diagnostics, DiagnosticLevel::Info, "model",
                   "Graph input " + shapeDescription(inputShape) +
                       " " + symbolicShapeDescription(inputSymbols) +
                       "; output " + shapeDescription(outputShape) + " " +
                       symbolicShapeDescription(outputSymbols));
  if (!matchesShape(inputShape, kInputShape) ||
      !matchesShape(outputShape, kOutputShape)) {
    setError(error, "The audio-separation model dimensions are incompatible: " +
                        shapeDescription(inputShape) + " -> " +
                        shapeDescription(outputShape) + ".");
    return false;
  }
  return true;
}

}  // namespace

struct BanditMaskModel::Impl {
  explicit Impl(DiagnosticReporter reporter)
      : diagnostics(std::move(reporter)),
        environment(ORT_LOGGING_LEVEL_WARNING, "radioify-separation",
                    onnxRuntimeLog, &diagnostics) {}

  DiagnosticReporter diagnostics;
  Ort::Env environment;
  std::unique_ptr<Ort::Session> session;
  Ort::MemoryInfo cpuMemory =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::vector<float> inputBuffer;
  std::vector<float> outputBuffer;
  Ort::Value inputValue{nullptr};
  Ort::Value outputValue{nullptr};

  void allocateIoBuffers() {
    inputBuffer.resize(kInputValues);
    outputBuffer.resize(kOutputValues);
    inputValue = Ort::Value::CreateTensor<float>(
        cpuMemory, inputBuffer.data(), inputBuffer.size(), kInputShape.data(),
        kInputShape.size());
    outputValue = Ort::Value::CreateTensor<float>(
        cpuMemory, outputBuffer.data(), outputBuffer.size(),
        kOutputShape.data(), kOutputShape.size());
  }
};

BanditMaskModel::BanditMaskModel() = default;
BanditMaskModel::~BanditMaskModel() = default;

bool BanditMaskModel::initialize(const std::filesystem::path& modelPath,
                                 DiagnosticReporter diagnostics,
                                 std::string* error) {
  if (error) error->clear();
  if (modelPath.empty()) {
    setError(error, "The audio-separation model path is empty.");
    return false;
  }
  try {
    auto implementation =
        std::make_unique<Impl>(std::move(diagnostics));
    reportDiagnostic(implementation->diagnostics, DiagnosticLevel::Info,
                     "onnxruntime", "Version: " + Ort::GetVersionString());
    std::vector<Ort::ConstEpDevice> directMlDevices;
    for (const Ort::ConstEpDevice& device :
         implementation->environment.GetEpDevices()) {
      if (std::string(device.EpName()) == "DmlExecutionProvider" &&
          device.Device().Type() == OrtHardwareDeviceType_GPU) {
        directMlDevices.push_back(device);
      }
    }
    if (directMlDevices.empty()) {
      setError(error,
               "No DirectML GPU is available; CPU audio separation is disabled.");
      return false;
    }

    const Ort::ConstHardwareDevice hardware = directMlDevices.front().Device();
    std::ostringstream deviceDescription;
    deviceDescription << "Selected DirectML GPU; vendor="
                      << (hardware.Vendor() ? hardware.Vendor() : "unknown")
                      << ", vendor_id=0x" << std::hex << hardware.VendorId()
                      << ", device_id=0x" << hardware.DeviceId();
    reportDiagnostic(implementation->diagnostics, DiagnosticLevel::Info,
                     "directml", deviceDescription.str());

    Ort::SessionOptions options;
    options.DisableMemPattern();
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    options.AddFreeDimensionOverrideByName("batch", kInputShape[0]);
    options.AddFreeDimensionOverrideByName("time", kInputShape[3]);
    Ort::KeyValuePairs providerOptions;
    options.AppendExecutionProvider_V2(implementation->environment,
                                       directMlDevices, providerOptions);
    implementation->session = std::make_unique<Ort::Session>(
        implementation->environment, modelPath.c_str(), options);
    if (!matchesModelContract(*implementation->session,
                              implementation->diagnostics, error)) {
      return false;
    }
    implementation->allocateIoBuffers();
    reportDiagnostic(implementation->diagnostics, DiagnosticLevel::Info,
                     "directml",
                     "Model session and reusable I/O buffers initialized.");
    impl_ = std::move(implementation);
    return true;
  } catch (const Ort::Exception& exception) {
    setError(error, std::string("Could not initialize DirectML separation: ") +
                        exception.what());
    return false;
  } catch (const std::exception& exception) {
    setError(error, std::string("Could not initialize audio separation: ") +
                        exception.what());
    return false;
  }
}

bool BanditMaskModel::run(std::span<const float> spectrogramRealImag,
                          std::span<const float>* masksRealImag,
                          const std::atomic<bool>* cancelRequested,
                          std::string* error) {
  if (error) error->clear();
  if (!impl_ || !impl_->session) {
    setError(error, "The DirectML separation session is not ready.");
    return false;
  }
  if (!masksRealImag) {
    setError(error, "No destination was provided for separation masks.");
    return false;
  }
  if (spectrogramRealImag.size() != kInputValues) {
    setError(error, "The DirectML separation session expected " +
                        std::to_string(kInputValues) +
                        " input values but received " +
                        std::to_string(spectrogramRealImag.size()) + ".");
    return false;
  }
  if (cancelled(cancelRequested)) {
    setError(error, "Audio separation cancelled.");
    return false;
  }
  try {
    std::copy(spectrogramRealImag.begin(), spectrogramRealImag.end(),
              impl_->inputBuffer.begin());
    const char* inputNames[] = {kInputName};
    const char* outputNames[] = {kOutputName};
    impl_->session->Run(Ort::RunOptions{nullptr}, inputNames,
                        &impl_->inputValue, 1, outputNames,
                        &impl_->outputValue, 1);
    if (cancelled(cancelRequested)) {
      setError(error, "Audio separation cancelled.");
      return false;
    }
    // The session contract and the bound output tensor were validated once at
    // initialization. Keep the repeated inference path allocation-free.
    *masksRealImag = std::span<const float>(impl_->outputBuffer);
    return true;
  } catch (const Ort::Exception& exception) {
    setError(error, std::string("DirectML audio separation failed: ") +
                        exception.what());
    return false;
  } catch (const std::exception& exception) {
    setError(error, std::string("Audio separation failed: ") +
                        exception.what());
    return false;
  }
}

}  // namespace audio_separation
