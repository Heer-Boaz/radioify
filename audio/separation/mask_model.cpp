#include "audio/separation/mask_model.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "audio/separation/artifact.h"
#include "audio/separation/spectral_transform.h"

namespace audio_separation {
namespace {

constexpr const char* kInputName = "spectrogram_ri";
constexpr const char* kOutputName = "masks_ri";

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

bool cancelled(const std::atomic<bool>* cancelRequested) {
  return cancelRequested &&
         cancelRequested->load(std::memory_order_relaxed);
}

bool isDimension(std::int64_t actual, std::int64_t expected) {
  return actual <= 0 || actual == expected;
}

bool matchesModelContract(Ort::Session& session, std::string* error) {
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
  const auto inputInfo = session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
  const auto outputInfo =
      session.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo();
  if (inputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
      outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
    setError(error,
             "The audio-separation model must use float32 graph I/O.");
    return false;
  }
  const std::vector<std::int64_t> inputShape = inputInfo.GetShape();
  const std::vector<std::int64_t> outputShape = outputInfo.GetShape();
  if (inputShape.size() != 5 || outputShape.size() != 6 ||
      !isDimension(inputShape[1], 1) ||
      !isDimension(inputShape[2], 1025) ||
      !isDimension(inputShape[4], 2) ||
      !isDimension(outputShape[1],
                   static_cast<std::int64_t>(kStemCount)) ||
      !isDimension(outputShape[2], 1) ||
      !isDimension(outputShape[3], 1025) ||
      !isDimension(outputShape[5], 2)) {
    setError(error, "The audio-separation model dimensions are incompatible.");
    return false;
  }
  return true;
}

}  // namespace

struct BanditMaskModel::Impl {
  Impl() : environment(ORT_LOGGING_LEVEL_WARNING, "radioify-separation") {}

  Ort::Env environment;
  std::unique_ptr<Ort::Session> session;
  Ort::MemoryInfo cpuMemory =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
};

BanditMaskModel::BanditMaskModel() = default;
BanditMaskModel::~BanditMaskModel() = default;

bool BanditMaskModel::initialize(const std::filesystem::path& modelPath,
                                 std::string* error) {
  if (error) error->clear();
  if (modelPath.empty()) {
    setError(error, "The audio-separation model path is empty.");
    return false;
  }
  try {
    auto implementation = std::make_unique<Impl>();
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

    Ort::SessionOptions options;
    options.DisableMemPattern();
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    Ort::KeyValuePairs providerOptions;
    options.AppendExecutionProvider_V2(implementation->environment,
                                       directMlDevices, providerOptions);
    implementation->session = std::make_unique<Ort::Session>(
        implementation->environment, modelPath.c_str(), options);
    if (!matchesModelContract(*implementation->session, error)) return false;
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

bool BanditMaskModel::run(const std::vector<float>& spectrogramRealImag,
                          std::vector<float>* masksRealImag,
                          const std::atomic<bool>* cancelRequested,
                          std::string* error) {
  if (!impl_ || !impl_->session || !masksRealImag ||
      spectrogramRealImag.size() !=
          BanditSpectralTransform::kRealImagValues) {
    setError(error, "The DirectML separation session is not ready.");
    return false;
  }
  if (cancelled(cancelRequested)) {
    setError(error, "Audio separation cancelled.");
    return false;
  }
  try {
    const std::array<std::int64_t, 5> inputShape = {
        1,
        1,
        static_cast<std::int64_t>(BanditSpectralTransform::kFrequencyBins),
        static_cast<std::int64_t>(BanditSpectralTransform::kTimeFrames),
        2};
    Ort::Value input = Ort::Value::CreateTensor<float>(
        impl_->cpuMemory, const_cast<float*>(spectrogramRealImag.data()),
        spectrogramRealImag.size(), inputShape.data(), inputShape.size());
    const char* inputNames[] = {kInputName};
    const char* outputNames[] = {kOutputName};
    std::vector<Ort::Value> output = impl_->session->Run(
        Ort::RunOptions{nullptr}, inputNames, &input, 1, outputNames, 1);
    if (cancelled(cancelRequested)) {
      setError(error, "Audio separation cancelled.");
      return false;
    }
    if (output.size() != 1 || !output[0].IsTensor()) {
      setError(error, "DirectML returned no audio-separation masks.");
      return false;
    }
    const std::size_t expectedValues =
        kStemCount * BanditSpectralTransform::kRealImagValues;
    const auto outputInfo = output[0].GetTensorTypeAndShapeInfo();
    if (outputInfo.GetElementCount() != expectedValues ||
        outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
      setError(error, "DirectML returned incompatible separation masks.");
      return false;
    }
    const float* values = output[0].GetTensorData<float>();
    masksRealImag->assign(values, values + expectedValues);
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
