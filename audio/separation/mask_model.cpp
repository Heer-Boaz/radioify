#include "audio/separation/mask_model.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <sstream>
#include <utility>

#include "audio/separation/artifact.h"
#include "audio/separation/inference_session.h"
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
  explicit Impl(std::unique_ptr<InferenceSessionFactory> selectedFactory)
      : factory(std::move(selectedFactory)) {}

  std::unique_ptr<InferenceSessionFactory> factory;
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
                                 const InferenceBackend& backend,
                                 DiagnosticReporter diagnostics,
                                 std::string* error,
                                 const ExecutionControl* control) {
  if (error) error->clear();
  if (modelPath.empty()) {
    setError(error, "The audio-separation model path is empty.");
    return false;
  }
  if (!backend.valid()) {
    setError(error, "The audio-separation inference backend is invalid.");
    return false;
  }
  try {
    std::unique_ptr<InferenceSessionFactory> factory =
        InferenceSessionFactory::create(backend, std::move(diagnostics),
                                        error);
    if (!factory) return false;
    auto implementation = std::make_unique<Impl>(std::move(factory));
    implementation->session =
        implementation->factory->loadModel(modelPath, control);
    if (!matchesModelContract(*implementation->session,
                              implementation->factory->diagnostics(), error)) {
      return false;
    }
    implementation->allocateIoBuffers();
    reportDiagnostic(implementation->factory->diagnostics(),
                     DiagnosticLevel::Info,
                     backend.diagnosticComponent,
                     "Model session and reusable I/O buffers initialized.");
    impl_ = std::move(implementation);
    return true;
  } catch (const Ort::Exception& exception) {
    setError(error, control && control->cancellationRequested()
                        ? "Audio separation cancelled."
                        : "Could not initialize " + backend.displayName +
                              " separation: " + exception.what());
    return false;
  } catch (const std::exception& exception) {
    setError(error, control && control->cancellationRequested()
                        ? "Audio separation cancelled."
                        : std::string("Could not initialize audio separation: ") +
                              exception.what());
    return false;
  }
}

MaskInferenceResult BanditMaskModel::run(
    std::span<const float> spectrogramRealImag,
    std::span<const float>* masksRealImag,
    const ExecutionControl& control, std::string* error) {
  if (error) error->clear();
  if (!impl_ || !impl_->session) {
    setError(error, "The audio-separation inference session is not ready.");
    return MaskInferenceResult::Failed;
  }
  if (!masksRealImag) {
    setError(error, "No destination was provided for separation masks.");
    return MaskInferenceResult::Failed;
  }
  if (spectrogramRealImag.size() != kInputValues) {
    setError(error, "The audio-separation session expected " +
                        std::to_string(kInputValues) +
                        " input values but received " +
                        std::to_string(spectrogramRealImag.size()) + ".");
    return MaskInferenceResult::Failed;
  }
  if (control.cancellationRequested()) {
    return MaskInferenceResult::Interrupted;
  }
  std::atomic<bool> interrupted{false};
  Ort::RunOptions runOptions;
  auto interruption = control.registerInterruption([&]() {
    interrupted.store(true, std::memory_order_relaxed);
    try {
      runOptions.SetTerminate();
    } catch (...) {
      // The local flag still prevents interrupted output from being consumed.
    }
  });
  try {
    std::copy(spectrogramRealImag.begin(), spectrogramRealImag.end(),
              impl_->inputBuffer.begin());
    const char* inputNames[] = {kInputName};
    const char* outputNames[] = {kOutputName};
    impl_->session->Run(runOptions, inputNames,
                        &impl_->inputValue, 1, outputNames,
                        &impl_->outputValue, 1);
    if (interrupted.load(std::memory_order_relaxed) ||
        control.cancellationRequested()) {
      return MaskInferenceResult::Interrupted;
    }
    // The session contract and the bound output tensor were validated once at
    // initialization. Keep the repeated inference path allocation-free.
    *masksRealImag = std::span<const float>(impl_->outputBuffer);
    return MaskInferenceResult::Succeeded;
  } catch (const Ort::Exception& exception) {
    if (interrupted.load(std::memory_order_relaxed)) {
      return MaskInferenceResult::Interrupted;
    }
    setError(error, impl_->factory->backend().displayName +
                        " audio separation failed: " + exception.what());
    return MaskInferenceResult::Failed;
  } catch (const std::exception& exception) {
    if (interrupted.load(std::memory_order_relaxed)) {
      return MaskInferenceResult::Interrupted;
    }
    setError(error, std::string("Audio separation failed: ") +
                        exception.what());
    return MaskInferenceResult::Failed;
  }
}

}  // namespace audio_separation
