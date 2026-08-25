#include "playback/video/transcript/transcriber.h"

#include <whisper.h>
#ifndef RADIOIFY_WHISPER_HAS_VULKAN
#define RADIOIFY_WHISPER_HAS_VULKAN 0
#endif
#if RADIOIFY_WHISPER_HAS_VULKAN
#include <ggml-vulkan.h>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ffmpegaudio.h"
#include "playback/video/transcript/document.h"
#include "runtime_helpers.h"

namespace playback_video_transcript {
namespace {

constexpr uint32_t kSampleRate = WHISPER_SAMPLE_RATE;
constexpr uint32_t kDecodeBlockFrames = 16 * 1024;
constexpr uint64_t kChunkFrames = 60ull * kSampleRate;
constexpr uint64_t kOverlapFrames = 4ull * kSampleRate;
constexpr const char* kDefaultModelName = "ggml-base-q5_1.bin";

struct WhisperContextDeleter {
  void operator()(whisper_context* context) const {
    if (context) whisper_free(context);
  }
};

using WhisperContextPtr =
    std::unique_ptr<whisper_context, WhisperContextDeleter>;

void discardWhisperLog(enum ggml_log_level, const char*, void*) {}

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

bool cancelled(const std::atomic<bool>* cancelRequested) {
  return cancelRequested &&
         cancelRequested->load(std::memory_order_relaxed);
}

void report(const ProgressCallback& callback, float fraction,
            const std::string& phase) {
  if (!callback) return;
  Progress progress;
  progress.fraction = std::clamp(fraction, 0.0f, 1.0f);
  progress.phase = phase;
  callback(progress);
}

float estimatedTranscriptionFraction(uint64_t processedFrames,
                                     uint64_t currentChunkFrames,
                                     int chunkProgress,
                                     uint64_t totalFrames) {
  const double within =
      std::clamp(static_cast<double>(chunkProgress) / 100.0, 0.0, 1.0);
  double audioFraction = 0.0;
  if (totalFrames > 0) {
    const double position = static_cast<double>(processedFrames) +
                            within * static_cast<double>(currentChunkFrames);
    audioFraction =
        position / static_cast<double>(std::max(totalFrames, uint64_t{1}));
  } else {
    const double completed = static_cast<double>(processedFrames) /
                             static_cast<double>(kChunkFrames);
    audioFraction = (completed + within) / (completed + 2.0);
  }
  return static_cast<float>(0.04 + 0.92 * std::clamp(audioFraction, 0.0, 1.0));
}

struct WhisperProgressBridge {
  const ProgressCallback* callback = nullptr;
  const std::atomic<bool>* cancelRequested = nullptr;
  uint64_t processedFrames = 0;
  uint64_t currentChunkFrames = 0;
  uint64_t totalFrames = 0;
};

void whisperProgress(whisper_context*, whisper_state*, int progress,
                     void* userData) {
  auto* bridge = static_cast<WhisperProgressBridge*>(userData);
  if (!bridge || !bridge->callback) return;
  report(*bridge->callback,
         estimatedTranscriptionFraction(
             bridge->processedFrames, bridge->currentChunkFrames, progress,
             bridge->totalFrames),
         "Transcribing audio");
}

bool whisperAbort(void* userData) {
  const auto* bridge = static_cast<const WhisperProgressBridge*>(userData);
  return bridge && cancelled(bridge->cancelRequested);
}

int inferenceThreadCount() {
  const unsigned int hardware = std::thread::hardware_concurrency();
  const unsigned int available = hardware == 0 ? 4u : hardware;
  return static_cast<int>(std::clamp(available, 1u, 8u));
}

int64_t framesToUs(uint64_t frames) {
  const uint64_t wholeSeconds = frames / kSampleRate;
  const uint64_t remainingFrames = frames % kSampleRate;
  constexpr uint64_t kUsPerSecond = 1000000;
  const uint64_t maxWholeSeconds =
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) /
      kUsPerSecond;
  if (wholeSeconds > maxWholeSeconds) {
    return std::numeric_limits<int64_t>::max();
  }
  const uint64_t timestampUs =
      wholeSeconds * kUsPerSecond +
      (remainingFrames * kUsPerSecond) / kSampleRate;
  if (timestampUs >
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return std::numeric_limits<int64_t>::max();
  }
  return static_cast<int64_t>(timestampUs);
}

std::string promptTail(const std::vector<Segment>& segments,
                       int64_t beforeUs) {
  constexpr size_t kMaxPromptBytes = 512;
  std::string prompt;
  for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
    if (it->endUs > beforeUs) continue;
    if (it->text.empty()) continue;
    const size_t separator = prompt.empty() ? 0 : 1;
    if (it->text.size() + separator + prompt.size() > kMaxPromptBytes) break;
    if (!prompt.empty()) prompt.insert(prompt.begin(), ' ');
    prompt.insert(0, it->text);
  }
  return prompt;
}

}  // namespace

std::filesystem::path resolveWhisperModelPath() {
  if (const auto configured = getEnvString("RADIOIFY_WHISPER_MODEL")) {
    const std::filesystem::path path = pathFromUtf8String(*configured);
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec) && !ec) return path;
  }

  for (const std::filesystem::path& root : radioifyResourceSearchRoots()) {
    const std::array<std::filesystem::path, 2> candidates = {
        root / "models" / kDefaultModelName,
        root / kDefaultModelName,
    };
    for (const std::filesystem::path& candidate : candidates) {
      std::error_code ec;
      if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
        return candidate;
      }
    }
  }
  return {};
}

bool createIndexedTranscript(const std::filesystem::path& videoPath,
                             const std::filesystem::path& outputPath,
                             const ProgressCallback& onProgress,
                             const std::atomic<bool>* cancelRequested,
                             std::string* error) {
  if (error) error->clear();
  if (videoPath.empty() || outputPath.empty()) {
    setError(error, "Video or transcript path is empty.");
    return false;
  }
  if (cancelled(cancelRequested)) {
    setError(error, "Transcript cancelled.");
    return false;
  }

  const std::filesystem::path modelPath = resolveWhisperModelPath();
  if (modelPath.empty()) {
    setError(error,
             "Whisper model not found. Set RADIOIFY_WHISPER_MODEL or rebuild "
             "Radioify so models/ggml-base-q5_1.bin is installed.");
    return false;
  }

  // whisper.cpp and ggml log backend discovery and model internals to stderr
  // by default, which would corrupt Radioify's live terminal surface.
  // User-visible backend selection and failures flow through task progress.
  whisper_log_set(discardWhisperLog, nullptr);

  std::string loadPhase = "Loading CPU speech model";
  int selectedGpuDevice = 0;
#if RADIOIFY_WHISPER_HAS_VULKAN
  const int vulkanDeviceCount = ggml_backend_vk_get_device_count();
  if (vulkanDeviceCount <= 0) {
    setError(error,
             "The Vulkan transcript backend is installed, but no Vulkan GPU "
             "is available.");
    return false;
  }
  size_t largestDeviceMemory = 0;
  for (int device = 0; device < vulkanDeviceCount; ++device) {
    size_t freeMemory = 0;
    size_t totalMemory = 0;
    ggml_backend_vk_get_device_memory(device, &freeMemory, &totalMemory);
    if (totalMemory > largestDeviceMemory) {
      largestDeviceMemory = totalMemory;
      selectedGpuDevice = device;
    }
  }
  std::array<char, 256> vulkanDeviceDescription{};
  ggml_backend_vk_get_device_description(
      selectedGpuDevice, vulkanDeviceDescription.data(),
      vulkanDeviceDescription.size());
  const std::string vulkanDevice = vulkanDeviceDescription.data();
  loadPhase = "Loading speech model on " +
              (vulkanDevice.empty() ? std::string("Vulkan GPU")
                                      : vulkanDevice);
#endif
  report(onProgress, 0.01f, loadPhase);
  const std::string modelPathUtf8 = toUtf8String(modelPath);
  whisper_context_params contextParams = whisper_context_default_params();
#if RADIOIFY_WHISPER_HAS_VULKAN
  contextParams.use_gpu = true;
  contextParams.gpu_device = selectedGpuDevice;
#else
  contextParams.use_gpu = false;
#endif
  // ggml Flash Attention's padded-mask precondition is not met for every
  // short decode window, so keep the stable GPU kernels for this workload.
  contextParams.flash_attn = false;
  WhisperContextPtr context(whisper_init_from_file_with_params(
      modelPathUtf8.c_str(), contextParams));
  if (!context) {
    setError(error, "Could not load Whisper model: " + modelPathUtf8);
    return false;
  }

  report(onProgress, 0.03f, "Opening video audio");
  FfmpegAudioDecoder decoder;
  std::string decodeError;
  if (!decoder.init(videoPath, 1, kSampleRate, &decodeError,
                    FfmpegAudioProbeMode::AudioOnly)) {
    setError(error, decodeError.empty() ? "Could not open the video's audio."
                                        : std::move(decodeError));
    return false;
  }

  uint64_t totalFrames = 0;
  decoder.getTotalFrames(&totalFrames);
  int64_t startOffsetFrames = 0;
  decoder.getStartOffsetFrames(&startOffsetFrames);
  const int64_t audioStartUs =
      startOffsetFrames >= 0
          ? framesToUs(static_cast<uint64_t>(startOffsetFrames))
          : -framesToUs(static_cast<uint64_t>(-startOffsetFrames));

  std::vector<float> decodeBlock(kDecodeBlockFrames);
  std::vector<float> chunk;
  chunk.reserve(static_cast<size_t>(kChunkFrames));
  std::vector<float> overlap;
  overlap.reserve(static_cast<size_t>(kOverlapFrames));
  std::vector<Segment> segments;
  uint64_t decodedFrames = 0;
  bool reachedEnd = false;
  bool firstChunk = true;

  while (!reachedEnd) {
    if (cancelled(cancelRequested)) {
      setError(error, "Transcript cancelled.");
      return false;
    }

    const uint64_t chunkStartFrame =
        decodedFrames - static_cast<uint64_t>(overlap.size());
    chunk.assign(overlap.begin(), overlap.end());
    const uint64_t leadingOverlapFrames =
        static_cast<uint64_t>(overlap.size());
    uint64_t newFramesRead = 0;
    report(onProgress,
           estimatedTranscriptionFraction(chunkStartFrame, 0, 0, totalFrames),
           "Decoding audio");
    while (chunk.size() < static_cast<size_t>(kChunkFrames)) {
      if (cancelled(cancelRequested)) {
        setError(error, "Transcript cancelled.");
        return false;
      }
      const uint64_t remaining =
          kChunkFrames - static_cast<uint64_t>(chunk.size());
      const uint32_t request = static_cast<uint32_t>(
          std::min<uint64_t>(remaining, decodeBlock.size()));
      uint64_t framesRead = 0;
      if (!decoder.readFrames(decodeBlock.data(), request, &framesRead)) {
        setError(error, "Could not decode the video's audio stream.");
        return false;
      }
      if (framesRead == 0) {
        reachedEnd = true;
        break;
      }
      decodedFrames += framesRead;
      newFramesRead += framesRead;
      chunk.insert(chunk.end(), decodeBlock.begin(),
                   decodeBlock.begin() + static_cast<ptrdiff_t>(framesRead));
    }

    if (newFramesRead == 0) break;

    int64_t seamUs = std::numeric_limits<int64_t>::min();
    if (!firstChunk) {
      const uint64_t seamFrame =
          chunkStartFrame + leadingOverlapFrames / 2;
      seamUs = std::max<int64_t>(0, audioStartUs + framesToUs(seamFrame));
      segments.erase(
          std::remove_if(segments.begin(), segments.end(),
                         [seamUs](const Segment& segment) {
                           const int64_t midpoint =
                               segment.startUs +
                               (segment.endUs - segment.startUs) / 2;
                           return midpoint >= seamUs;
                         }),
          segments.end());
    }

    WhisperProgressBridge bridge;
    bridge.callback = &onProgress;
    bridge.cancelRequested = cancelRequested;
    bridge.processedFrames = chunkStartFrame;
    bridge.currentChunkFrames = static_cast<uint64_t>(chunk.size());
    bridge.totalFrames = totalFrames;

    const int64_t chunkStartUs = framesToUs(chunkStartFrame);
    const int64_t promptCutoffUs =
        std::max<int64_t>(0, audioStartUs + chunkStartUs);
    std::string prompt = promptTail(segments, promptCutoffUs);
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

    report(onProgress,
           estimatedTranscriptionFraction(chunkStartFrame, chunk.size(), 0,
                                          totalFrames),
           "Transcribing audio");
    const int result = whisper_full(context.get(), parameters, chunk.data(),
                                    static_cast<int>(chunk.size()));
    if (result != 0) {
      if (cancelled(cancelRequested)) {
        setError(error, "Transcript cancelled.");
      } else {
        setError(error, "Whisper could not transcribe the video's audio.");
      }
      return false;
    }

    const int segmentCount = whisper_full_n_segments(context.get());
    for (int index = 0; index < segmentCount; ++index) {
      if (whisper_full_get_segment_no_speech_prob(context.get(), index) >=
          parameters.no_speech_thold) {
        continue;
      }
      const char* text = whisper_full_get_segment_text(context.get(), index);
      if (!text || !isMeaningfulTranscriptText(text)) continue;
      const int64_t localStartUs =
          whisper_full_get_segment_t0(context.get(), index) * 10000;
      const int64_t localEndUs =
          whisper_full_get_segment_t1(context.get(), index) * 10000;
      Segment segment;
      segment.startUs =
          std::max<int64_t>(0, audioStartUs + chunkStartUs + localStartUs);
      segment.endUs =
          std::max(segment.startUs + 1000,
                   audioStartUs + chunkStartUs + localEndUs);
      const int64_t midpoint =
          segment.startUs + (segment.endUs - segment.startUs) / 2;
      if (!firstChunk && midpoint < seamUs) continue;
      segment.text = text;
      segments.push_back(std::move(segment));
    }

    if (!reachedEnd) {
      const size_t overlapCount = static_cast<size_t>(
          std::min<uint64_t>(kOverlapFrames, chunk.size()));
      overlap.assign(chunk.end() - static_cast<ptrdiff_t>(overlapCount),
                     chunk.end());
    }
    firstChunk = false;
  }

  if (decodedFrames == 0) {
    setError(error, "The video has no decodable audio samples.");
    return false;
  }
  if (cancelled(cancelRequested)) {
    setError(error, "Transcript cancelled.");
    return false;
  }

  report(onProgress, 0.98f, "Writing indexed transcript");
  if (!writeIndexedTranscript(outputPath, segments, error)) return false;
  report(onProgress, 1.0f, "Transcript complete");
  return true;
}

}  // namespace playback_video_transcript
