#include "playback/video/transcript/transcriber.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ffmpegaudio.h"
#include "core/file_output.h"
#include "core/file_instance.h"
#include "core/path_identity.h"
#include "playback/video/transcript/artifact.h"
#include "playback/video/transcript/document.h"
#include "playback/video/transcript/provenance.h"
#include "playback/video/transcript/subtitle_cues.h"
#include "playback/video/transcript/whisper_engine.h"
#include "runtime_helpers.h"

namespace playback_video_transcript {
namespace {

#ifndef RADIOIFY_WHISPER_MODEL_SHA256
#define RADIOIFY_WHISPER_MODEL_SHA256 ""
#endif

constexpr uint32_t kSampleRate = WhisperEngine::kSampleRate;
constexpr uint32_t kDecodeBlockFrames = 16 * 1024;
constexpr uint64_t kChunkFrames = 60ull * kSampleRate;
// The midpoint seam retains eight seconds of model context on either side.
// DTW word alignment is unreliable in the first few seconds of an isolated
// chunk, especially when speech follows silence; a short overlap allowed that
// boundary artifact to leak into otherwise valid SRT timing.
constexpr uint64_t kOverlapFrames = 16ull * kSampleRate;
constexpr const char *kDefaultModelName = "ggml-base-q5_1.bin";

struct WhisperModelSelection {
  std::filesystem::path path;
  WhisperAlignmentPreset alignmentPreset = WhisperAlignmentPreset::None;
  std::string sourceLanguage;
  bool packagedDefault = false;
};

void setError(std::string *error, std::string message) {
  if (error)
    *error = std::move(message);
}

bool cancelled(const std::atomic<bool> *cancelRequested) {
  return cancelRequested && cancelRequested->load(std::memory_order_relaxed);
}

void report(const ProgressCallback &callback, float fraction,
            const std::string &phase) {
  if (!callback)
    return;
  Progress progress;
  progress.fraction = std::clamp(fraction, 0.0f, 1.0f);
  progress.phase = phase;
  callback(progress);
}

float estimatedTranscriptionFraction(uint64_t processedFrames,
                                     uint64_t currentChunkFrames,
                                     int chunkProgress, uint64_t totalFrames) {
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
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / kUsPerSecond;
  if (wholeSeconds > maxWholeSeconds) {
    return std::numeric_limits<int64_t>::max();
  }
  const uint64_t timestampUs = wholeSeconds * kUsPerSecond +
                               (remainingFrames * kUsPerSecond) / kSampleRate;
  if (timestampUs >
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return std::numeric_limits<int64_t>::max();
  }
  return static_cast<int64_t>(timestampUs);
}

bool resolveWhisperModel(WhisperModelSelection *selection, std::string *error) {
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
    selection->packagedDefault = true;
    for (const std::filesystem::path &root : radioifyResourceSearchRoots()) {
      const std::array<std::filesystem::path, 2> candidates = {
          root / "models" / kDefaultModelName,
          root / kDefaultModelName,
      };
      for (const std::filesystem::path &candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
          selection->path = candidate;
          selection->alignmentPreset = WhisperAlignmentPreset::Base;
          break;
        }
      }
      if (!selection->path.empty())
        break;
    }
    if (selection->path.empty()) {
      setError(error, "Whisper model not found. Rebuild Radioify so "
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
  if (const auto configuredLanguage =
          getEnvString("RADIOIFY_WHISPER_LANGUAGE")) {
    const auto language = normalizeWhisperLanguageOverride(*configuredLanguage);
    if (!language) {
      setError(error,
               "RADIOIFY_WHISPER_LANGUAGE is invalid. Use auto or a "
               "two-letter source-language code such as en, nl, de, or fr.");
      return false;
    }
    selection->sourceLanguage = *language;
  }
  return true;
}

std::string producerIdentity(const WhisperModelSelection &selection,
                             TranscriptLanguageMode languageMode) {
  std::error_code error;
  const std::uintmax_t size =
      std::filesystem::file_size(selection.path, error);
  if (error)
    return {};
  const auto modified = std::filesystem::last_write_time(selection.path, error);
  if (error)
    return {};
  const auto instance = fileInstanceIdentity(selection.path);
  std::ostringstream identity;
  identity << "radioify-indexed-transcript-v2\n"
           << (languageMode == TranscriptLanguageMode::TranslateToEnglish
                   ? "translate-to-english\n"
                   : "source-language\n")
           << kSampleRate << '\n'
           << kChunkFrames << '\n'
           << kOverlapFrames << '\n'
           << static_cast<int>(selection.alignmentPreset) << '\n'
           << (selection.sourceLanguage.empty() ? "auto"
                                                : selection.sourceLanguage)
           << '\n';
  if (selection.packagedDefault) {
    identity << "sha256:" << RADIOIFY_WHISPER_MODEL_SHA256;
  } else {
    identity << "custom:"
             << toUtf8String(pathIdentityKey(makePathIdentity(selection.path)))
             << '\n'
             << size << '\n'
             << static_cast<std::int64_t>(
                    modified.time_since_epoch().count())
             << '\n'
             << (instance ? instance->device : 0) << '\n'
             << (instance ? instance->file : 0);
  }
  return identity.str();
}

} // namespace

std::optional<std::string> automaticEnglishTranscriptProducerIdentity(
    std::string *error) {
  WhisperModelSelection selection;
  if (!resolveWhisperModel(&selection, error))
    return std::nullopt;
  std::string identity = producerIdentity(
      selection, TranscriptLanguageMode::TranslateToEnglish);
  if (identity.empty()) {
    setError(error, "Could not establish the Whisper producer identity.");
    return std::nullopt;
  }
  return identity;
}

struct IndexedTranscriptOperation::Impl {
  std::filesystem::path videoPath;
  std::filesystem::path outputPath;
  TranscriptPublishMode publishMode = TranscriptPublishMode::ReplaceExisting;
  TranscriptLanguageMode languageMode = TranscriptLanguageMode::SourceLanguage;
  std::optional<WhisperModelSelection> model;
  std::unique_ptr<WhisperEngine> whisper;
  std::unique_ptr<FfmpegAudioDecoder> decoder;
  std::string sourceLanguage;
  std::vector<float> decodeBlock;
  std::vector<float> chunk;
  std::vector<float> overlap;
  std::vector<Segment> segments;
  uint64_t totalFrames = 0;
  uint64_t decodedFrames = 0;
  uint64_t chunkStartFrame = 0;
  uint64_t leadingOverlapFrames = 0;
  uint64_t newFramesRead = 0;
  int64_t audioStartUs = 0;
  bool reachedEnd = false;
  bool firstChunk = true;
  bool chunkActive = false;
  bool complete = false;
  std::filesystem::path publishedPath;
  std::optional<TranscriptSourceIdentity> sourceIdentity;
  std::string producerIdentity;

  bool matches(const std::filesystem::path &video,
               const std::filesystem::path &output,
               TranscriptPublishMode requestedPublishMode,
               TranscriptLanguageMode requestedLanguageMode) const {
    return videoPath.lexically_normal() == video.lexically_normal() &&
           outputPath.lexically_normal() == output.lexically_normal() &&
           publishMode == requestedPublishMode &&
           languageMode == requestedLanguageMode;
  }
};

namespace {

bool operationCancelled(const TranscriptOperationControl &control) {
  return control.cancelled && control.cancelled();
}

bool operationGpuAllowed(const TranscriptOperationControl &control) {
  return !control.backgroundGpuAllowed || control.backgroundGpuAllowed();
}

TranscriptOperationResult
interruptedResult(const TranscriptOperationControl &control) {
  if (operationCancelled(control)) {
    return {TranscriptOperationStatus::Cancelled, "Transcript cancelled.", {}};
  }
  return {TranscriptOperationStatus::Yielded, {}, {}};
}

TranscriptOperationResult failedResult(std::string detail) {
  return {TranscriptOperationStatus::Failed, std::move(detail), {}};
}

} // namespace

IndexedTranscriptOperation::IndexedTranscriptOperation()
    : impl_(std::make_unique<Impl>()) {}

IndexedTranscriptOperation::~IndexedTranscriptOperation() = default;

void IndexedTranscriptOperation::reset() { impl_ = std::make_unique<Impl>(); }

TranscriptOperationResult IndexedTranscriptOperation::resume(
    const std::filesystem::path &videoPath,
    const std::filesystem::path &outputPath, TranscriptPublishMode publishMode,
    TranscriptLanguageMode languageMode,
    const TranscriptOperationControl &control,
    const TranscriptCommitStarted &outputCommitStarted) {
  if (videoPath.empty() || outputPath.empty())
    return failedResult("Video or transcript path is empty.");
  if (operationCancelled(control) || !operationGpuAllowed(control))
    return interruptedResult(control);

  if (!impl_->videoPath.empty() &&
      !impl_->matches(videoPath, outputPath, publishMode, languageMode)) {
    return failedResult(
        "A transcript transaction cannot be resumed for another video.");
  }
  if (impl_->complete) {
    return {TranscriptOperationStatus::Succeeded, {}, impl_->publishedPath};
  }

  if (impl_->videoPath.empty()) {
    impl_->videoPath = videoPath;
    impl_->outputPath = outputPath;
    impl_->publishMode = publishMode;
    impl_->languageMode = languageMode;
    std::string identityError;
    impl_->sourceIdentity =
        captureTranscriptSourceIdentity(videoPath, &identityError);
    if (!impl_->sourceIdentity)
      return failedResult(std::move(identityError));
  }

  const auto interruption = [&]() {
    if (impl_->whisper) {
      const std::string detected = impl_->whisper->sourceLanguage();
      if (!detected.empty())
        impl_->sourceLanguage = detected;
      // Yielding gives foreground playback both execution priority and the
      // Vulkan allocation. Decoded PCM remains owner-held, so only the model
      // context must be recreated when background work is admitted again.
      impl_->whisper.reset();
    }
    return interruptedResult(control);
  };

  if (!impl_->model) {
    WhisperModelSelection model;
    std::string modelError;
    if (!resolveWhisperModel(&model, &modelError))
      return failedResult(std::move(modelError));
    impl_->model = std::move(model);
    impl_->producerIdentity =
        producerIdentity(*impl_->model, impl_->languageMode);
    if (impl_->producerIdentity.empty())
      return failedResult("Could not establish the Whisper producer identity.");
  }

  if (!control.runSpeechChunk && !impl_->whisper) {
    std::string modelError;
    report(control.progress, 0.01f, "Loading Vulkan speech model");
    impl_->whisper = std::make_unique<WhisperEngine>();
    std::string vulkanDevice;
    const std::string language = !impl_->sourceLanguage.empty()
                                     ? impl_->sourceLanguage
                                     : impl_->model->sourceLanguage;
    if (!impl_->whisper->initialize(impl_->model->path,
                                    impl_->model->alignmentPreset, language,
                                    &vulkanDevice,
                                    &modelError)) {
      impl_->whisper.reset();
      return failedResult(std::move(modelError));
    }
    report(control.progress, 0.02f, "Vulkan ready on " + vulkanDevice);
    if (operationCancelled(control) || !operationGpuAllowed(control))
      return interruption();
  }

  if (!impl_->decoder) {
    report(control.progress, 0.03f, "Opening video audio");
    impl_->decoder = std::make_unique<FfmpegAudioDecoder>();
    std::string decodeError;
    if (!impl_->decoder->init(videoPath, 1, kSampleRate, &decodeError)) {
      impl_->decoder.reset();
      return failedResult(decodeError.empty()
                              ? "Could not open the video's audio."
                              : std::move(decodeError));
    }
    impl_->decoder->getTotalFrames(&impl_->totalFrames);
    int64_t startOffsetFrames = 0;
    impl_->decoder->getStartOffsetFrames(&startOffsetFrames);
    impl_->audioStartUs =
        startOffsetFrames >= 0
            ? framesToUs(static_cast<uint64_t>(startOffsetFrames))
            : -framesToUs(static_cast<uint64_t>(-startOffsetFrames));
    impl_->decodeBlock.resize(kDecodeBlockFrames);
    impl_->chunk.reserve(static_cast<size_t>(kChunkFrames));
    impl_->overlap.reserve(static_cast<size_t>(kOverlapFrames));
  }

  for (;;) {
    if (operationCancelled(control) || !operationGpuAllowed(control))
      return interruption();

    if (!impl_->chunkActive) {
      if (impl_->reachedEnd)
        break;
      impl_->chunkStartFrame =
          impl_->decodedFrames - static_cast<uint64_t>(impl_->overlap.size());
      impl_->chunk.assign(impl_->overlap.begin(), impl_->overlap.end());
      impl_->leadingOverlapFrames =
          static_cast<uint64_t>(impl_->overlap.size());
      impl_->newFramesRead = 0;
      impl_->chunkActive = true;
    }

    report(control.progress,
           estimatedTranscriptionFraction(impl_->decodedFrames, 0, 0,
                                          impl_->totalFrames),
           "Decoding audio");
    while (impl_->chunk.size() < static_cast<size_t>(kChunkFrames)) {
      if (operationCancelled(control) || !operationGpuAllowed(control))
        return interruption();
      const uint64_t remaining =
          kChunkFrames - static_cast<uint64_t>(impl_->chunk.size());
      const uint32_t request = static_cast<uint32_t>(
          std::min<uint64_t>(remaining, impl_->decodeBlock.size()));
      uint64_t framesRead = 0;
      if (!impl_->decoder->readFrames(impl_->decodeBlock.data(), request,
                                      &framesRead)) {
        return failedResult("Could not decode the video's audio stream.");
      }
      if (framesRead == 0) {
        impl_->reachedEnd = true;
        break;
      }
      impl_->decodedFrames += framesRead;
      impl_->newFramesRead += framesRead;
      impl_->chunk.insert(impl_->chunk.end(), impl_->decodeBlock.begin(),
                          impl_->decodeBlock.begin() +
                              static_cast<ptrdiff_t>(framesRead));
    }

    if (impl_->newFramesRead == 0) {
      impl_->chunkActive = false;
      break;
    }

    int64_t seamUs = std::numeric_limits<int64_t>::min();
    if (!impl_->firstChunk) {
      const uint64_t seamFrame =
          impl_->chunkStartFrame + impl_->leadingOverlapFrames / 2;
      seamUs =
          std::max<int64_t>(0, impl_->audioStartUs + framesToUs(seamFrame));
    }
    const uint64_t processedFrames =
        impl_->chunkStartFrame + impl_->leadingOverlapFrames;
    const uint64_t currentChunkFrames =
        static_cast<uint64_t>(impl_->chunk.size()) -
        impl_->leadingOverlapFrames;
    const int64_t chunkStartUs = framesToUs(impl_->chunkStartFrame);
    report(control.progress,
           estimatedTranscriptionFraction(processedFrames, currentChunkFrames,
                                          0, impl_->totalFrames),
           languageMode == TranscriptLanguageMode::TranslateToEnglish
               ? "Translating speech to English"
               : "Transcribing audio");

    std::vector<RecognizedSegment> recognizedSegments;
    std::string inferenceError;
    bool cancellationObserved = false;
    bool gpuRevocationObserved = false;
    const auto interrupted = [&]() {
      cancellationObserved =
          cancellationObserved || operationCancelled(control);
      gpuRevocationObserved =
          gpuRevocationObserved || !operationGpuAllowed(control);
      return cancellationObserved || gpuRevocationObserved;
    };
    const auto chunkProgress = [&](int progressValue) {
          report(control.progress,
                 estimatedTranscriptionFraction(
                     processedFrames, currentChunkFrames, progressValue,
                     impl_->totalFrames),
                 languageMode == TranscriptLanguageMode::TranslateToEnglish
                     ? "Translating speech to English"
                     : "Transcribing audio");
        };
    bool recognized = false;
    if (control.runSpeechChunk) {
      SpeechWorkerRequest request;
      request.model = impl_->model->path;
      request.alignmentPreset = impl_->model->alignmentPreset;
      request.sourceLanguage = !impl_->sourceLanguage.empty()
                                   ? impl_->sourceLanguage
                                   : impl_->model->sourceLanguage;
      request.task = languageMode == TranscriptLanguageMode::TranslateToEnglish
                         ? WhisperTask::TranslateToEnglish
                         : WhisperTask::Transcribe;
      request.samples = impl_->chunk.data();
      request.sampleCount = impl_->chunk.size();
      SpeechWorkerResult worker =
          control.runSpeechChunk(request, chunkProgress);
      if (worker.status == SpeechWorkerStatus::Yielded)
        return interruption();
      if (worker.status == SpeechWorkerStatus::Cancelled) {
        return {TranscriptOperationStatus::Cancelled,
                worker.detail.empty() ? "Transcript cancelled."
                                      : std::move(worker.detail),
                {}};
      }
      if (worker.status == SpeechWorkerStatus::Failed) {
        return failedResult(worker.detail.empty()
                                ? "Whisper worker could not transcribe audio."
                                : std::move(worker.detail));
      }
      recognizedSegments = std::move(worker.document.segments);
      if (!worker.document.sourceLanguage.empty())
        impl_->sourceLanguage = std::move(worker.document.sourceLanguage);
      recognized = true;
    } else {
      recognized = impl_->whisper->transcribe(
          impl_->chunk.data(), impl_->chunk.size(), chunkProgress, interrupted,
          &recognizedSegments, &inferenceError,
          languageMode == TranscriptLanguageMode::TranslateToEnglish
              ? WhisperTask::TranslateToEnglish
              : WhisperTask::Transcribe);
    }
    if (!recognized) {
      if (cancellationObserved || gpuRevocationObserved || interrupted()) {
        if (cancellationObserved) {
          impl_->whisper.reset();
          return {TranscriptOperationStatus::Cancelled,
                  "Transcript cancelled.", {}};
        }
        return interruption();
      }
      return failedResult(
          inferenceError.empty()
              ? "Whisper could not transcribe the video's audio."
              : std::move(inferenceError));
    }

    // A chunk becomes visible to the operation only after Whisper completed.
    // If GPU priority interrupted inference, this exact decoded chunk is kept
    // and retried without approximate seeking or partial cue mutation.
    if (!impl_->firstChunk) {
      impl_->segments.erase(
          std::remove_if(impl_->segments.begin(), impl_->segments.end(),
                         [seamUs](const Segment &segment) {
                           const int64_t midpoint =
                               segment.startUs +
                               (segment.endUs - segment.startUs) / 2;
                           return midpoint >= seamUs;
                         }),
          impl_->segments.end());
    }
    const std::vector<Segment> recognizedCues =
        buildSubtitleCues(recognizedSegments);
    if (impl_->whisper) {
      const std::string detectedLanguage = impl_->whisper->sourceLanguage();
      if (!detectedLanguage.empty())
        impl_->sourceLanguage = detectedLanguage;
    }
    for (const Segment &recognizedCue : recognizedCues) {
      Segment segment;
      segment.startUs = std::max<int64_t>(
          0, impl_->audioStartUs + chunkStartUs + recognizedCue.startUs);
      segment.endUs =
          std::max(segment.startUs + 1000,
                   impl_->audioStartUs + chunkStartUs + recognizedCue.endUs);
      const int64_t midpoint =
          segment.startUs + (segment.endUs - segment.startUs) / 2;
      if (!impl_->firstChunk && midpoint < seamUs)
        continue;
      segment.text = recognizedCue.text;
      impl_->segments.push_back(std::move(segment));
    }

    if (!impl_->reachedEnd) {
      const size_t overlapCount = static_cast<size_t>(
          std::min<uint64_t>(kOverlapFrames, impl_->chunk.size()));
      impl_->overlap.assign(impl_->chunk.end() -
                                static_cast<ptrdiff_t>(overlapCount),
                            impl_->chunk.end());
    } else {
      impl_->overlap.clear();
    }
    impl_->firstChunk = false;
    impl_->chunkActive = false;
    impl_->chunk.clear();
  }

  if (impl_->decodedFrames == 0)
    return failedResult("The video has no decodable audio samples.");
  if (operationCancelled(control))
    return interruptedResult(control);

  finalizeSubtitleCueTimeline(&impl_->segments);
  if (impl_->segments.empty()) {
    return {TranscriptOperationStatus::NoSpeech,
            "Speech transcription found no usable dialogue.", {}};
  }
  report(control.progress, 0.98f, "Saving transcript");
  std::filesystem::path publishedPath = impl_->outputPath;
  if (impl_->languageMode == TranscriptLanguageMode::TranslateToEnglish) {
    const std::filesystem::path generated =
        generatedEnglishTranscriptPathForVideo(impl_->videoPath);
    if (!generated.empty())
      publishedPath = generated;
  } else if (impl_->outputPath.lexically_normal() ==
      transcriptPathForVideo(impl_->videoPath).lexically_normal()) {
    const std::string language = !impl_->sourceLanguage.empty()
                                     ? impl_->sourceLanguage
                                     : impl_->model->sourceLanguage;
    const std::filesystem::path languagePath =
        languageTaggedTranscriptPathForVideo(impl_->videoPath, language);
    if (!languagePath.empty())
      publishedPath = languagePath;
  }
  std::string publishError;
  if (!impl_->sourceIdentity ||
      !transcriptSourceMatches(*impl_->sourceIdentity, impl_->videoPath)) {
    return failedResult(
        "The source video changed while its transcript was being prepared.");
  }

  file_output::PublishMode outputMode = file_output::PublishMode::CreateNew;
  if (impl_->publishMode == TranscriptPublishMode::ReplaceExisting) {
    outputMode = file_output::PublishMode::ReplaceExisting;
  } else if (impl_->publishMode == TranscriptPublishMode::ReplaceOwned) {
    if (!isGeneratedEnglishTranscriptPath(impl_->videoPath, publishedPath)) {
      return failedResult(
          "Background transcript replacement requires a Radioify-owned "
          "destination.");
    }
    // The private filename is the ownership boundary. A stale pair or a
    // transcript left behind before its provenance commit must be repairable
    // after source replacement or process termination.
    outputMode = file_output::PublishMode::ReplaceExisting;
  }

  const std::filesystem::path provenancePath =
      transcriptProvenancePath(publishedPath);
  std::vector<file_output::TransactionDestination> destinations = {
      {publishedPath, outputMode}, {provenancePath, outputMode}};
  auto transaction =
      file_output::TransactionGroup::begin(std::move(destinations),
                                           &publishError);
  if (!transaction)
    return failedResult(std::move(publishError));
  if (!writeIndexedTranscriptStaging(transaction->temporaryPath(0),
                                     impl_->segments, &publishError) ||
      !writeTranscriptProvenanceStaging(
          *impl_->sourceIdentity, impl_->producerIdentity, publishedPath,
          transaction->temporaryPath(0), transaction->temporaryPath(1),
          &publishError)) {
    return failedResult(std::move(publishError));
  }
  if (outputCommitStarted && !outputCommitStarted())
    return failedResult("Transcript cancelled before publication.");
  if (!transcriptSourceMatches(*impl_->sourceIdentity, impl_->videoPath)) {
    return failedResult(
        "The source video changed while its transcript was being prepared.");
  }
  if (!transaction->publish(&publishError))
    return failedResult(std::move(publishError));
  impl_->publishedPath = publishedPath;
  impl_->complete = true;
  report(control.progress, 1.0f, "Transcript complete");
  return {TranscriptOperationStatus::Succeeded, {}, publishedPath};
}

bool createIndexedTranscript(const std::filesystem::path &videoPath,
                             const std::filesystem::path &outputPath,
                             TranscriptPublishMode publishMode,
                             const ProgressCallback &onProgress,
                             const std::atomic<bool> *cancelRequested,
                             std::string *error,
                             const TranscriptCommitStarted &outputCommitStarted,
                             std::filesystem::path *publishedPathResult) {
  if (error)
    error->clear();
  IndexedTranscriptOperation operation;
  TranscriptOperationControl control;
  control.cancelled = [cancelRequested]() {
    return cancelled(cancelRequested);
  };
  control.backgroundGpuAllowed = []() { return true; };
  control.progress = onProgress;
  TranscriptOperationResult result = operation.resume(
      videoPath, outputPath, publishMode,
      TranscriptLanguageMode::SourceLanguage, control, outputCommitStarted);
  if (result.status != TranscriptOperationStatus::Succeeded) {
    setError(error, result.detail.empty() ? "Transcript did not complete."
                                          : std::move(result.detail));
    return false;
  }
  if (publishedPathResult)
    *publishedPathResult = std::move(result.publishedPath);
  return true;
}

} // namespace playback_video_transcript
