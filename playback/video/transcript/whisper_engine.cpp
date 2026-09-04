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

namespace playback_video_transcript {
namespace {

static_assert(WhisperEngine::kSampleRate == WHISPER_SAMPLE_RATE,
              "WhisperEngine's PCM contract must match whisper.cpp");

void setError(std::string *error, std::string message) {
  if (error)
    *error = std::move(message);
}

void discardWhisperLog(enum ggml_log_level, const char *, void *) {}

void configureWhisperLogging() {
  static std::once_flag configured;
  std::call_once(configured, []() {
    // Both APIs are process-global. Install one stable callback once so model
    // and backend diagnostics cannot corrupt Radioify's live terminal surface.
    whisper_log_set(discardWhisperLog, nullptr);
    ggml_log_set(discardWhisperLog, nullptr);
  });
}

bool chooseRuntimeVulkanDevice(VulkanDeviceCandidate *selected,
                               std::string *error) {
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
    if (!isGpu)
      continue;

    const int currentWhisperGpuIndex = whisperGpuIndex++;
    if (ggml_backend_dev_backend_reg(device) != vulkanRegistration) {
      continue;
    }

    VulkanDeviceCandidate candidate;
    candidate.whisperGpuIndex = currentWhisperGpuIndex;
    candidate.deviceClass = type == GGML_BACKEND_DEVICE_TYPE_GPU
                                ? VulkanDeviceClass::Discrete
                                : VulkanDeviceClass::Integrated;
    ggml_backend_dev_memory(device, &candidate.freeMemory,
                            &candidate.totalMemory);
    if (const char *name = ggml_backend_dev_name(device)) {
      candidate.name = name;
    }
    if (const char *description = ggml_backend_dev_description(device)) {
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

whisper_alignment_heads_preset
whisperAlignmentPreset(WhisperAlignmentPreset preset) {
  switch (preset) {
  case WhisperAlignmentPreset::None:
    return WHISPER_AHEADS_NONE;
  case WhisperAlignmentPreset::TinyEn:
    return WHISPER_AHEADS_TINY_EN;
  case WhisperAlignmentPreset::Tiny:
    return WHISPER_AHEADS_TINY;
  case WhisperAlignmentPreset::BaseEn:
    return WHISPER_AHEADS_BASE_EN;
  case WhisperAlignmentPreset::Base:
    return WHISPER_AHEADS_BASE;
  case WhisperAlignmentPreset::SmallEn:
    return WHISPER_AHEADS_SMALL_EN;
  case WhisperAlignmentPreset::Small:
    return WHISPER_AHEADS_SMALL;
  case WhisperAlignmentPreset::MediumEn:
    return WHISPER_AHEADS_MEDIUM_EN;
  case WhisperAlignmentPreset::Medium:
    return WHISPER_AHEADS_MEDIUM;
  case WhisperAlignmentPreset::LargeV1:
    return WHISPER_AHEADS_LARGE_V1;
  case WhisperAlignmentPreset::LargeV2:
    return WHISPER_AHEADS_LARGE_V2;
  case WhisperAlignmentPreset::LargeV3:
    return WHISPER_AHEADS_LARGE_V3;
  case WhisperAlignmentPreset::LargeV3Turbo:
    return WHISPER_AHEADS_LARGE_V3_TURBO;
  }
  return WHISPER_AHEADS_NONE;
}

struct WhisperCallbackBridge {
  const WhisperEngine::ProgressCallback *progress = nullptr;
  const WhisperEngine::AbortCheck *abort = nullptr;
};

void whisperProgress(whisper_context *, whisper_state *, int progress,
                     void *userData) {
  const auto *bridge = static_cast<const WhisperCallbackBridge *>(userData);
  if (bridge && bridge->progress && *bridge->progress) {
    (*bridge->progress)(progress);
  }
}

bool whisperAbort(void *userData) {
  const auto *bridge = static_cast<const WhisperCallbackBridge *>(userData);
  return bridge && bridge->abort && *bridge->abort && (*bridge->abort)();
}

int64_t whisperTimestampUs(int64_t centiseconds) {
  if (centiseconds < 0)
    return -1;
  constexpr int64_t kUsPerCentisecond = 10'000;
  if (centiseconds >
      (std::numeric_limits<int64_t>::max)() / kUsPerCentisecond) {
    return (std::numeric_limits<int64_t>::max)();
  }
  return centiseconds * kUsPerCentisecond;
}

struct WhisperContextDeleter {
  void operator()(whisper_context *context) const {
    if (context)
      whisper_free(context);
  }
};

using WhisperContextPtr =
    std::unique_ptr<whisper_context, WhisperContextDeleter>;

} // namespace

struct WhisperEngine::Impl {
  WhisperContextPtr context;
  // The transcriber feeds one media file in overlapping chunks. Once a chunk
  // contains recognized speech, retain its detected language so later music,
  // effects, or short utterances cannot make auto-detection switch scripts.
  std::string sourceLanguage;
};

WhisperEngine::WhisperEngine() = default;
WhisperEngine::~WhisperEngine() = default;

bool WhisperEngine::initialize(const std::filesystem::path &modelPath,
                               WhisperAlignmentPreset alignmentPreset,
                               std::string sourceLanguage,
                               std::string *deviceDescription,
                               std::string *error) {
  impl_.reset();
  if (deviceDescription)
    deviceDescription->clear();
  if (error)
    error->clear();
  if (modelPath.empty()) {
    setError(error, "Whisper model path is empty.");
    return false;
  }
  if (!sourceLanguage.empty()) {
    const int languageId = whisper_lang_id(sourceLanguage.c_str());
    const char *canonicalLanguage =
        languageId >= 0 ? whisper_lang_str(languageId) : nullptr;
    if (!canonicalLanguage) {
      setError(error, "Whisper does not support source language '" +
                          sourceLanguage + "'.");
      return false;
    }
    sourceLanguage = canonicalLanguage;
  }

  configureWhisperLogging();

  VulkanDeviceCandidate device;
  if (!chooseRuntimeVulkanDevice(&device, error))
    return false;

  whisper_context_params parameters = whisper_context_default_params();
  parameters.use_gpu = true;
  parameters.require_gpu = true;
  parameters.gpu_device = device.whisperGpuIndex;
  // GGML Flash Attention's padded-mask precondition is not met for every
  // short decode window, so keep the stable Vulkan kernels for this workload.
  parameters.flash_attn = false;
  if (alignmentPreset != WhisperAlignmentPreset::None) {
    parameters.dtw_token_timestamps = true;
    parameters.dtw_aheads_preset = whisperAlignmentPreset(alignmentPreset);
  }

  const auto modelPathBytes = modelPath.u8string();
  const std::string modelPathUtf8(modelPathBytes.begin(),
                                  modelPathBytes.end());
  WhisperContextPtr context(
      whisper_init_from_file_with_params(modelPathUtf8.c_str(), parameters));

  if (!context) {
    setError(error,
             "Could not initialize Whisper with the required Vulkan GPU and "
             "model: " +
                 modelPathUtf8);
    return false;
  }

  auto impl = std::make_unique<Impl>();
  impl->context = std::move(context);
  impl->sourceLanguage = std::move(sourceLanguage);
  std::string selectedDescription =
      device.description.empty() ? device.name : device.description;
  if (selectedDescription.empty())
    selectedDescription = "Vulkan GPU";
  if (deviceDescription)
    *deviceDescription = std::move(selectedDescription);
  impl_ = std::move(impl);
  return true;
}

bool WhisperEngine::transcribe(const float *samples, size_t sampleCount,
                               const ProgressCallback &onProgress,
                               const AbortCheck &shouldAbort,
                               std::vector<RecognizedSegment> *segments,
                               std::string *error, WhisperTask task) {
  if (error)
    error->clear();
  if (segments)
    segments->clear();
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
  if (task == WhisperTask::TranslateToEnglish &&
      !whisper_is_multilingual(impl_->context.get())) {
    setError(error,
             "Automatic English speech evidence requires a multilingual "
             "Whisper model.");
    return false;
  }

  WhisperCallbackBridge bridge;
  bridge.progress = &onProgress;
  bridge.abort = &shouldAbort;

  whisper_full_params parameters =
      whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  parameters.n_threads = inferenceThreadCount();
  parameters.translate = task == WhisperTask::TranslateToEnglish;
  parameters.language =
      impl_->sourceLanguage.empty() ? "auto" : impl_->sourceLanguage.c_str();
  // "auto" detects a language and then transcribes it. In whisper.cpp,
  // detect_language=true is a detection-only mode that returns no segments.
  parameters.detect_language = false;
  parameters.no_timestamps = false;
  parameters.single_segment = false;
  parameters.print_special = false;
  parameters.print_progress = false;
  parameters.print_realtime = false;
  parameters.print_timestamps = false;
  // Indexed transcripts are speech navigation, not SDH. Ask Whisper to
  // suppress its native music/noise tokens before decoding; the transcript
  // policy still rejects any complete [sound annotation] emitted lexically.
  parameters.suppress_nst = true;
  // Recognition owns word alignment; subtitle_cues owns presentation
  // segmentation. Enabling token timestamps without Whisper's max_len split
  // keeps those responsibilities separate and prevents model-created orphan
  // words at an arbitrary character boundary.
  // Whisper's translation task does not provide trustworthy word-level
  // alignment. Preserve its source-audio segment intervals and only request
  // DTW/token timing for same-language transcription.
  parameters.token_timestamps = task == WhisperTask::Transcribe;
  if (parameters.token_timestamps)
    parameters.thold_pt = 0.01f;
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
    const char *text =
        whisper_full_get_segment_text(impl_->context.get(), index);
    if (!text)
      continue;

    RecognizedSegment segment;
    segment.startUs = whisperTimestampUs(
        whisper_full_get_segment_t0(impl_->context.get(), index));
    segment.endUs = whisperTimestampUs(
        whisper_full_get_segment_t1(impl_->context.get(), index));
    if (segment.startUs < 0 || segment.endUs <= segment.startUs)
      continue;
    segment.text = text;

    const int tokenCount = parameters.token_timestamps
                               ? whisper_full_n_tokens(impl_->context.get(),
                                                       index)
                               : 0;
    segment.tokens.reserve(static_cast<size_t>(std::max(0, tokenCount)));
    const whisper_token firstSpecialToken =
        whisper_token_eot(impl_->context.get());
    for (int tokenIndex = 0; tokenIndex < tokenCount; ++tokenIndex) {
      const whisper_token tokenId =
          whisper_full_get_token_id(impl_->context.get(), index, tokenIndex);
      if (tokenId >= firstSpecialToken)
        continue;
      const char *tokenText =
          whisper_full_get_token_text(impl_->context.get(), index, tokenIndex);
      if (!tokenText || *tokenText == '\0')
        continue;
      const whisper_token_data tokenData =
          whisper_full_get_token_data(impl_->context.get(), index, tokenIndex);
      RecognizedToken token;
      token.startUs = whisperTimestampUs(tokenData.t0);
      token.endUs = whisperTimestampUs(tokenData.t1);
      token.alignmentUs = whisperTimestampUs(tokenData.t_dtw);
      token.text = tokenText;
      segment.tokens.push_back(std::move(token));
    }
    segments->push_back(std::move(segment));
  }
  if (impl_->sourceLanguage.empty() && !segments->empty()) {
    const int languageId = whisper_full_lang_id(impl_->context.get());
    if (const char *language = whisper_lang_str(languageId)) {
      impl_->sourceLanguage = language;
    }
  }
  return true;
}

std::string WhisperEngine::sourceLanguage() const {
  return impl_ ? impl_->sourceLanguage : std::string{};
}

} // namespace playback_video_transcript
