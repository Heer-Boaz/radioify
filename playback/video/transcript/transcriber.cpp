#include "playback/video/transcript/transcriber.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "ffmpegaudio.h"
#include "playback/video/transcript/document.h"
#include "playback/video/transcript/subtitle_cues.h"
#include "playback/video/transcript/whisper_engine.h"
#include "runtime_helpers.h"

namespace playback_video_transcript {
namespace {

constexpr uint32_t kSampleRate = WhisperEngine::kSampleRate;
constexpr uint32_t kDecodeBlockFrames = 16 * 1024;
constexpr uint64_t kChunkFrames = 60ull * kSampleRate;
// The midpoint seam retains eight seconds of model context on either side.
// DTW word alignment is unreliable in the first few seconds of an isolated
// chunk, especially when speech follows silence; a short overlap allowed that
// boundary artifact to leak into otherwise valid SRT timing.
constexpr uint64_t kOverlapFrames = 16ull * kSampleRate;
constexpr const char* kDefaultModelName = "ggml-base-q5_1.bin";

struct WhisperModelSelection {
  std::filesystem::path path;
  WhisperAlignmentPreset alignmentPreset = WhisperAlignmentPreset::None;
};

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

bool resolveWhisperModel(WhisperModelSelection* selection,
                         std::string* error) {
  if (!selection) {
    setError(error, "No destination was provided for Whisper model selection.");
    return false;
  }
  *selection = {};
  if (const auto configured = getEnvString("RADIOIFY_WHISPER_MODEL")) {
    selection->path = pathFromUtf8String(*configured);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(selection->path, ec) || ec) {
      setError(error, "RADIOIFY_WHISPER_MODEL is not a readable model file: " +
                          *configured);
      return false;
    }
  } else {
    for (const std::filesystem::path& root : radioifyResourceSearchRoots()) {
      const std::array<std::filesystem::path, 2> candidates = {
          root / "models" / kDefaultModelName,
          root / kDefaultModelName,
      };
      for (const std::filesystem::path& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
          selection->path = candidate;
          selection->alignmentPreset = WhisperAlignmentPreset::Base;
          break;
        }
      }
      if (!selection->path.empty()) break;
    }
    if (selection->path.empty()) {
      setError(error,
               "Whisper model not found. Rebuild Radioify so "
               "models/ggml-base-q5_1.bin is installed, or set "
               "RADIOIFY_WHISPER_MODEL.");
      return false;
    }
  }

  if (const auto configuredPreset =
          getEnvString("RADIOIFY_WHISPER_DTW_PRESET")) {
    const auto preset = parseWhisperAlignmentPreset(*configuredPreset);
    if (!preset) {
      setError(error,
               "RADIOIFY_WHISPER_DTW_PRESET is invalid. Use none, tiny, "
               "tiny.en, base, base.en, small, small.en, medium, medium.en, "
               "large.v1, large.v2, large.v3, or large.v3.turbo.");
      return false;
    }
    selection->alignmentPreset = *preset;
  }
  return true;
}

}  // namespace

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

  WhisperModelSelection model;
  if (!resolveWhisperModel(&model, error)) return false;

  report(onProgress, 0.01f, "Loading Vulkan speech model");
  WhisperEngine whisper;
  std::string vulkanDevice;
  if (!whisper.initialize(model.path, model.alignmentPreset, &vulkanDevice,
                          error)) {
    return false;
  }
  report(onProgress, 0.02f, "Vulkan ready on " + vulkanDevice);

  report(onProgress, 0.03f, "Opening video audio");
  FfmpegAudioDecoder decoder;
  std::string decodeError;
  if (!decoder.init(videoPath, 1, kSampleRate, &decodeError)) {
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
           estimatedTranscriptionFraction(decodedFrames, 0, 0, totalFrames),
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

    const uint64_t processedFrames =
        chunkStartFrame + leadingOverlapFrames;
    const uint64_t currentChunkFrames =
        static_cast<uint64_t>(chunk.size()) - leadingOverlapFrames;

    const int64_t chunkStartUs = framesToUs(chunkStartFrame);
    report(onProgress,
           estimatedTranscriptionFraction(
               processedFrames, currentChunkFrames, 0, totalFrames),
           "Transcribing audio");
    std::vector<RecognizedSegment> recognizedSegments;
    std::string inferenceError;
    if (!whisper.transcribe(
            chunk.data(), chunk.size(),
            [&](int progress) {
              report(onProgress,
                     estimatedTranscriptionFraction(
                         processedFrames, currentChunkFrames, progress,
                         totalFrames),
                     "Transcribing audio");
            },
            [cancelRequested]() { return cancelled(cancelRequested); },
            &recognizedSegments, &inferenceError)) {
      setError(error, inferenceError.empty()
                          ? "Whisper could not transcribe the video's audio."
                          : std::move(inferenceError));
      return false;
    }

    const std::vector<Segment> recognizedCues =
        buildSubtitleCues(recognizedSegments);
    for (const Segment& recognized : recognizedCues) {
      Segment segment;
      segment.startUs =
          std::max<int64_t>(0,
                            audioStartUs + chunkStartUs + recognized.startUs);
      segment.endUs =
          std::max(segment.startUs + 1000,
                   audioStartUs + chunkStartUs + recognized.endUs);
      const int64_t midpoint =
          segment.startUs + (segment.endUs - segment.startUs) / 2;
      if (!firstChunk && midpoint < seamUs) continue;
      segment.text = recognized.text;
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

  finalizeSubtitleCueTimeline(&segments);
  report(onProgress, 0.98f, "Writing indexed transcript");
  if (!writeIndexedTranscript(outputPath, segments, error)) return false;
  report(onProgress, 1.0f, "Transcript complete");
  return true;
}

}  // namespace playback_video_transcript
