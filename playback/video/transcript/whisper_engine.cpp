#include "playback/video/transcript/whisper_engine.h"

#include <ggml-backend.h>
#include <ggml-vulkan.h>
#include <whisper.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#include "playback/video/transcript/device_selection.h"
#include "runtime_helpers.h"

namespace playback_video_transcript {
namespace {

static_assert(WhisperEngine::kSampleRate == WHISPER_SAMPLE_RATE,
              "WhisperEngine's PCM contract must match whisper.cpp");

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

void discardWhisperLog(enum ggml_log_level, const char*, void*) {}

void configureWhisperLogging() {
  static std::once_flag configured;
  std::call_once(configured, []() {
    // Both APIs are process-global. Install one stable callback once so model
    // and backend diagnostics cannot corrupt Radioify's live terminal surface.
    whisper_log_set(discardWhisperLog, nullptr);
    ggml_log_set(discardWhisperLog, nullptr);
  });
}

bool chooseRuntimeVulkanDevice(VulkanDeviceCandidate* selected,
                               std::string* error) {
  if (!selected) {
    setError(error, "No destination was provided for Vulkan selection.");
    return false;
  }

  ggml_backend_reg_t vulkanRegistration =
      ggml_backend_reg_by_name(GGML_VK_NAME);
  if (!vulkanRegistration) {
    setError(error, "The Vulkan backend is not registered with GGML.");
    return false;
  }

  std::vector<VulkanDeviceCandidate> policyCandidates;
  int whisperGpuIndex = 0;
  for (size_t index = 0; index < ggml_backend_dev_count(); ++index) {
    ggml_backend_dev_t device = ggml_backend_dev_get(index);
    const enum ggml_backend_dev_type type = ggml_backend_dev_type(device);
    const bool isGpu = type == GGML_BACKEND_DEVICE_TYPE_GPU ||
                       type == GGML_BACKEND_DEVICE_TYPE_IGPU;
    if (!isGpu) continue;

    const int currentWhisperGpuIndex = whisperGpuIndex++;
    if (ggml_backend_dev_backend_reg(device) != vulkanRegistration) {
      continue;
    }

    VulkanDeviceCandidate candidate;
    candidate.whisperGpuIndex = currentWhisperGpuIndex;
    candidate.deviceClass =
        type == GGML_BACKEND_DEVICE_TYPE_GPU
            ? VulkanDeviceClass::Discrete
            : VulkanDeviceClass::Integrated;
    ggml_backend_dev_memory(device, &candidate.freeMemory,
                            &candidate.totalMemory);
    if (const char* name = ggml_backend_dev_name(device)) {
      candidate.name = name;
    }
    if (const char* description = ggml_backend_dev_description(device)) {
      candidate.description = description;
    }
    policyCandidates.push_back(std::move(candidate));
  }

  const std::optional<VulkanDeviceCandidate> preferred =
      selectPreferredVulkanDevice(policyCandidates);
  if (!preferred) {
    setError(error, "No Vulkan GPU is visible to Whisper.");
    return false;
  }

  *selected = *preferred;
  return true;
}

int inferenceThreadCount() {
  const unsigned int hardware = std::thread::hardware_concurrency();
  const unsigned int available = hardware == 0 ? 4u : hardware;
  return static_cast<int>(std::clamp(available, 1u, 8u));
}

struct WhisperCallbackBridge {
  const WhisperEngine::ProgressCallback* progress = nullptr;
  const WhisperEngine::AbortCheck* abort = nullptr;
};

void whisperProgress(whisper_context*, whisper_state*, int progress,
                     void* userData) {
  const auto* bridge = static_cast<const WhisperCallbackBridge*>(userData);
  if (bridge && bridge->progress && *bridge->progress) {
    (*bridge->progress)(progress);
  }
}

bool whisperAbort(void* userData) {
  const auto* bridge = static_cast<const WhisperCallbackBridge*>(userData);
  return bridge && bridge->abort && *bridge->abort && (*bridge->abort)();
}

struct WhisperContextDeleter {
  void operator()(whisper_context* context) const {
    if (context) whisper_free(context);
  }
};

using WhisperContextPtr =
    std::unique_ptr<whisper_context, WhisperContextDeleter>;

}  // namespace

struct WhisperEngine::Impl {
  WhisperContextPtr context;
};

WhisperEngine::WhisperEngine() = default;
WhisperEngine::~WhisperEngine() = default;

bool WhisperEngine::initialize(const std::filesystem::path& modelPath,
                               std::string* deviceDescription,
                               std::string* error) {
  impl_.reset();
  if (deviceDescription) deviceDescription->clear();
  if (error) error->clear();
  if (modelPath.empty()) {
    setError(error, "Whisper model path is empty.");
    return false;
  }

  configureWhisperLogging();

  VulkanDeviceCandidate device;
  if (!chooseRuntimeVulkanDevice(&device, error)) return false;

  whisper_context_params parameters = whisper_context_default_params();
  parameters.use_gpu = true;
  parameters.require_gpu = true;
  parameters.gpu_device = device.whisperGpuIndex;
  // GGML Flash Attention's padded-mask precondition is not met for every
  // short decode window, so keep the stable Vulkan kernels for this workload.
  parameters.flash_attn = false;

  const std::string modelPathUtf8 = toUtf8String(modelPath);
  WhisperContextPtr context(whisper_init_from_file_with_params(
      modelPathUtf8.c_str(), parameters));

  if (!context) {
    setError(error,
             "Could not initialize Whisper with the required Vulkan GPU and "
             "model: " +
                 modelPathUtf8);
    return false;
  }

  auto impl = std::make_unique<Impl>();
  impl->context = std::move(context);
  std::string selectedDescription =
      device.description.empty() ? device.name : device.description;
  if (selectedDescription.empty()) selectedDescription = "Vulkan GPU";
  if (deviceDescription) *deviceDescription = std::move(selectedDescription);
  impl_ = std::move(impl);
  return true;
}

bool WhisperEngine::transcribe(
    const float* samples, size_t sampleCount, const std::string& prompt,
    const ProgressCallback& onProgress, const AbortCheck& shouldAbort,
    std::vector<RecognizedSegment>* segments, std::string* error) {
  if (error) error->clear();
  if (segments) segments->clear();
  if (!impl_ || !impl_->context) {
    setError(error, "Whisper is not initialized.");
    return false;
  }
  if (!samples || sampleCount == 0 || !segments) {
    setError(error, "No audio samples were provided to Whisper.");
    return false;
  }
  if (sampleCount > static_cast<size_t>(std::numeric_limits<int>::max())) {
    setError(error, "The Whisper audio chunk is too large.");
    return false;
  }

  WhisperCallbackBridge bridge;
  bridge.progress = &onProgress;
  bridge.abort = &shouldAbort;

  whisper_full_params parameters =
      whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  parameters.n_threads = inferenceThreadCount();
  parameters.translate = false;
  parameters.language = "auto";
  // "auto" detects a language and then transcribes it. In whisper.cpp,
  // detect_language=true is a detection-only mode that returns no segments.
  parameters.detect_language = false;
  parameters.no_timestamps = false;
  parameters.single_segment = false;
  parameters.print_special = false;
  parameters.print_progress = false;
  parameters.print_realtime = false;
  parameters.print_timestamps = false;
  parameters.max_len = 80;
  parameters.split_on_word = true;
  parameters.initial_prompt = prompt.empty() ? nullptr : prompt.c_str();
  parameters.progress_callback = whisperProgress;
  parameters.progress_callback_user_data = &bridge;
  parameters.abort_callback = whisperAbort;
  parameters.abort_callback_user_data = &bridge;

  const int result = whisper_full(impl_->context.get(), parameters, samples,
                                  static_cast<int>(sampleCount));
  if (result != 0) {
    setError(error, shouldAbort && shouldAbort()
                        ? "Transcript cancelled."
                        : "Whisper could not transcribe the audio chunk.");
    return false;
  }

  const int segmentCount = whisper_full_n_segments(impl_->context.get());
  segments->reserve(static_cast<size_t>(std::max(0, segmentCount)));
  for (int index = 0; index < segmentCount; ++index) {
    if (whisper_full_get_segment_no_speech_prob(impl_->context.get(), index) >=
        parameters.no_speech_thold) {
      continue;
    }
    const char* text =
        whisper_full_get_segment_text(impl_->context.get(), index);
    if (!text) continue;

    RecognizedSegment segment;
    segment.startUs =
        whisper_full_get_segment_t0(impl_->context.get(), index) * 10000;
    segment.endUs =
        whisper_full_get_segment_t1(impl_->context.get(), index) * 10000;
    segment.text = text;
    segments->push_back(std::move(segment));
  }
  return true;
}

}  // namespace playback_video_transcript
