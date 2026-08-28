#include "audio/separation/separator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

#include "audio/ffmpegaudio.h"
#include "audio/flac_writer.h"
#include "audio/separation/mask_model.h"
#include "audio/separation/spectral_transform.h"
#include "runtime_helpers.h"

namespace audio_separation {
namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kChannels = 2;
constexpr std::uint64_t kChunkFrames =
    static_cast<std::uint64_t>(BanditSpectralTransform::kChunkFrames);
constexpr std::uint64_t kChunkHopFrames = kSampleRate;
constexpr std::uint64_t kFrontPadFrames = 14ull * kSampleRate;
constexpr std::uint32_t kDecodeBlockFrames = 64 * 1024;
constexpr const char* kDefaultModelName =
    "bandit-v2-multi-mask-core-fp16.onnx";

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

bool cancelled(const std::atomic<bool>* cancelRequested) {
  return cancelRequested &&
         cancelRequested->load(std::memory_order_relaxed);
}

void report(const ProgressCallback& callback, float fraction,
            std::string phase) {
  if (!callback) return;
  callback({std::clamp(fraction, 0.0f, 1.0f), std::move(phase)});
}

std::uint64_t ceilDivide(std::uint64_t value, std::uint64_t divisor) {
  return value / divisor + (value % divisor == 0 ? 0 : 1);
}

class ScopedTemporaryFile {
 public:
  explicit ScopedTemporaryFile(std::filesystem::path path)
      : path_(std::move(path)) {}
  ~ScopedTemporaryFile() {
    if (path_.empty()) return;
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  ScopedTemporaryFile(const ScopedTemporaryFile&) = delete;
  ScopedTemporaryFile& operator=(const ScopedTemporaryFile&) = delete;

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

bool resolveBundledModelPath(std::filesystem::path* modelPath,
                             std::string* error) {
  if (!modelPath) {
    setError(error, "No audio-separation model destination was provided.");
    return false;
  }
  modelPath->clear();
  for (const std::filesystem::path& root : radioifyResourceSearchRoots()) {
    const std::array<std::filesystem::path, 3> candidates = {
        root / "models" / "audio_separation" / kDefaultModelName,
        root / "models" / kDefaultModelName,
        root / kDefaultModelName};
    for (const std::filesystem::path& candidate : candidates) {
      std::error_code ec;
      if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
        *modelPath = candidate;
        return true;
      }
    }
  }
  setError(error,
           "Bundled audio-separation model not found. Reinstall Radioify or "
           "rebuild it so models/audio_separation/"
           "bandit-v2-multi-mask-core-fp16.onnx is installed.");
  return false;
}

bool validateRequest(const std::filesystem::path& mediaPath,
                     const std::atomic<bool>* cancelRequested,
                     std::string* error) {
  if (mediaPath.empty()) {
    setError(error, "The media path is empty.");
    return false;
  }
  if (cancelled(cancelRequested)) {
    setError(error, "Audio separation cancelled.");
    return false;
  }
  return true;
}

bool validateModelPath(const std::filesystem::path& modelPath,
                       std::string* error) {
  if (modelPath.empty()) {
    setError(error, "No audio-separation model was selected.");
    return false;
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(modelPath, ec) || ec) {
    setError(error, "Audio-separation model is not a readable file: " +
                        toUtf8String(modelPath));
    return false;
  }
  return true;
}

bool decodeToRawFile(const std::filesystem::path& mediaPath,
                     const std::filesystem::path& rawPath,
                     const ProgressCallback& onProgress,
                     const std::atomic<bool>* cancelRequested,
                     std::uint64_t* outputFrames, std::string* error) {
  if (!outputFrames) return false;
  *outputFrames = 0;
  FfmpegAudioDecoder decoder;
  std::string decodeError;
  if (!decoder.init(mediaPath, kChannels, kSampleRate, &decodeError)) {
    setError(error, decodeError.empty() ? "Could not open the media audio."
                                        : std::move(decodeError));
    return false;
  }
  std::uint64_t expectedFrames = 0;
  decoder.getTotalFrames(&expectedFrames);
  std::uint64_t initialPadding = 0;
  std::uint64_t trailingPadding = 0;
  decoder.getPaddingFrames(&initialPadding, &trailingPadding);

  std::ofstream raw(rawPath, std::ios::binary | std::ios::trunc);
  if (!raw) {
    setError(error, "Could not create temporary audio storage beside the media file.");
    return false;
  }
  std::vector<float> block(
      static_cast<std::size_t>(kDecodeBlockFrames) * kChannels);
  std::uint64_t decodedFrames = 0;
  std::uint64_t skippedFrames = 0;
  std::uint64_t writtenFrames = 0;
  for (;;) {
    if (cancelled(cancelRequested)) {
      setError(error, "Audio separation cancelled.");
      return false;
    }
    std::uint64_t framesRead = 0;
    if (!decoder.readFrames(block.data(), kDecodeBlockFrames, &framesRead)) {
      setError(error, "Could not decode the media audio.");
      return false;
    }
    if (framesRead == 0) break;
    decodedFrames += framesRead;
    const std::uint64_t skip =
        std::min(framesRead, initialPadding - skippedFrames);
    skippedFrames += skip;
    const std::uint64_t framesToWrite = framesRead - skip;
    if (framesToWrite > 0) {
      const float* first = block.data() + skip * kChannels;
      const std::size_t bytes =
          static_cast<std::size_t>(framesToWrite) * kChannels * sizeof(float);
      raw.write(reinterpret_cast<const char*>(first),
                static_cast<std::streamsize>(bytes));
      if (!raw) {
        setError(error, "Temporary audio storage ran out of writable space.");
        return false;
      }
      writtenFrames += framesToWrite;
    }
    if (expectedFrames > 0) {
      const double fraction = static_cast<double>(decodedFrames) /
                              static_cast<double>(expectedFrames +
                                                  initialPadding +
                                                  trailingPadding);
      report(onProgress, static_cast<float>(0.03 + 0.04 * fraction),
             "Decoding 48 kHz stereo audio");
    }
  }
  raw.close();
  if (!raw) {
    setError(error, "Could not finish temporary audio storage.");
    return false;
  }
  if (writtenFrames <= trailingPadding) {
    setError(error, "The media file contains no separable audio frames.");
    return false;
  }
  writtenFrames -= trailingPadding;
  const std::uintmax_t finalBytes =
      static_cast<std::uintmax_t>(writtenFrames) * kChannels * sizeof(float);
  std::error_code resizeError;
  std::filesystem::resize_file(rawPath, finalBytes, resizeError);
  if (resizeError) {
    setError(error, "Could not trim codec padding from temporary audio: " +
                        resizeError.message());
    return false;
  }
  *outputFrames = writtenFrames;
  return true;
}

enum class SegmentKind : std::uint8_t {
  Zero,
  Forward,
  Reverse,
};

struct PaddedSegment {
  SegmentKind kind = SegmentKind::Zero;
  std::uint64_t paddedStart = 0;
  std::uint64_t length = 0;
  std::uint64_t sourceStart = 0;
};

struct ChunkPlan {
  std::vector<PaddedSegment> segments;
  std::uint64_t chunkCount = 0;
};

void appendSegment(ChunkPlan* plan, SegmentKind kind, std::uint64_t length,
                   std::uint64_t sourceStart = 0) {
  if (!plan || length == 0) return;
  const std::uint64_t start = plan->segments.empty()
                                  ? 0
                                  : plan->segments.back().paddedStart +
                                        plan->segments.back().length;
  plan->segments.push_back({kind, start, length, sourceStart});
}

ChunkPlan buildChunkPlan(std::uint64_t sourceFrames) {
  ChunkPlan plan;
  plan.chunkCount =
      ceilDivide(kFrontPadFrames + sourceFrames, kChunkHopFrames);
  const std::uint64_t paddedFrames =
      (plan.chunkCount - 1) * kChunkHopFrames + kChunkFrames;
  const std::uint64_t rightPadding =
      paddedFrames - kFrontPadFrames - sourceFrames;

  if (sourceFrames <= kFrontPadFrames) {
    const std::uint64_t reflected = sourceFrames > 0 ? sourceFrames - 1 : 0;
    appendSegment(&plan, SegmentKind::Zero,
                  kFrontPadFrames - reflected);
    if (reflected > 0) {
      appendSegment(&plan, SegmentKind::Reverse, reflected, reflected);
    }
  } else {
    appendSegment(&plan, SegmentKind::Reverse, kFrontPadFrames,
                  kFrontPadFrames);
  }
  appendSegment(&plan, SegmentKind::Forward, sourceFrames, 0);
  const std::uint64_t reflectedRight =
      std::min(rightPadding, sourceFrames > 0 ? sourceFrames - 1 : 0);
  if (reflectedRight > 0) {
    appendSegment(&plan, SegmentKind::Reverse, reflectedRight,
                  sourceFrames - 2);
  }
  appendSegment(&plan, SegmentKind::Zero,
                rightPadding - reflectedRight);
  return plan;
}

class RawAudioReader {
 public:
  bool open(const std::filesystem::path& path, std::string* error) {
    stream_.open(path, std::ios::binary);
    if (stream_) return true;
    setError(error, "Could not reopen temporary audio storage.");
    return false;
  }

  bool fillChunk(const ChunkPlan& plan, std::uint64_t paddedStart,
                 std::vector<float>* destination, std::string* error) {
    if (!destination) return false;
    destination->assign(static_cast<std::size_t>(kChunkFrames) * kChannels,
                        0.0f);
    const std::uint64_t paddedEnd = paddedStart + kChunkFrames;
    for (const PaddedSegment& segment : plan.segments) {
      const std::uint64_t segmentEnd = segment.paddedStart + segment.length;
      const std::uint64_t overlapStart =
          std::max(paddedStart, segment.paddedStart);
      const std::uint64_t overlapEnd = std::min(paddedEnd, segmentEnd);
      if (overlapStart >= overlapEnd || segment.kind == SegmentKind::Zero) {
        continue;
      }
      const std::uint64_t segmentOffset = overlapStart - segment.paddedStart;
      const std::uint64_t destinationOffset = overlapStart - paddedStart;
      const std::uint64_t frameCount = overlapEnd - overlapStart;
      if (segment.kind == SegmentKind::Forward) {
        if (!readForward(segment.sourceStart + segmentOffset, frameCount,
                         destination->data() + destinationOffset * kChannels,
                         error)) {
          return false;
        }
      } else {
        const std::uint64_t firstSource =
            segment.sourceStart - segmentOffset;
        const std::uint64_t lastSource = firstSource - (frameCount - 1);
        scratch_.resize(static_cast<std::size_t>(frameCount) * kChannels);
        if (!readForward(lastSource, frameCount, scratch_.data(), error)) {
          return false;
        }
        float* output = destination->data() + destinationOffset * kChannels;
        for (std::uint64_t frame = 0; frame < frameCount; ++frame) {
          const std::uint64_t reversed = frameCount - 1 - frame;
          for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
            output[frame * kChannels + channel] =
                scratch_[reversed * kChannels + channel];
          }
        }
      }
    }
    return true;
  }

 private:
  bool readForward(std::uint64_t sourceFrame, std::uint64_t frameCount,
                   float* destination, std::string* error) {
    if (sourceFrame >
        static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) /
            (kChannels * sizeof(float))) {
      setError(error, "The temporary audio offset is too large.");
      return false;
    }
    const std::streamoff offset = static_cast<std::streamoff>(
        sourceFrame * kChannels * sizeof(float));
    const std::uint64_t byteCount = frameCount * kChannels * sizeof(float);
    if (byteCount > static_cast<std::uint64_t>(
                        std::numeric_limits<std::streamsize>::max())) {
      setError(error, "The temporary audio read is too large.");
      return false;
    }
    stream_.clear();
    stream_.seekg(offset, std::ios::beg);
    stream_.read(reinterpret_cast<char*>(destination),
                 static_cast<std::streamsize>(byteCount));
    if (stream_) return true;
    setError(error, "Could not read temporary audio storage.");
    return false;
  }

  std::ifstream stream_;
  std::vector<float> scratch_;
};

std::vector<float> chunkWindow() {
  std::vector<float> window(static_cast<std::size_t>(kChunkFrames));
  constexpr double kScaler =
      static_cast<double>(kChunkFrames) /
      (2.0 * static_cast<double>(kChunkHopFrames));
  for (std::size_t index = 0; index < window.size(); ++index) {
    const double angle =
        2.0 * std::numbers::pi * static_cast<double>(index) /
        static_cast<double>(window.size());
    window[index] =
        static_cast<float>((0.5 - 0.5 * std::cos(angle)) / kScaler);
  }
  return window;
}

bool writeCroppedHop(std::uint64_t globalStart, std::uint64_t sourceFrames,
                     const std::array<std::vector<float>, kStemCount>& overlap,
                     std::array<audio_file::FlacWriter, kStemCount>* writers,
                     std::uint64_t* writtenFrames, std::string* error) {
  if (!writers || !writtenFrames) return false;
  const std::uint64_t globalEnd = globalStart + kChunkHopFrames;
  const std::uint64_t cropStart = kFrontPadFrames;
  const std::uint64_t cropEnd = cropStart + sourceFrames;
  const std::uint64_t outputStart = std::max(globalStart, cropStart);
  const std::uint64_t outputEnd = std::min(globalEnd, cropEnd);
  if (outputStart >= outputEnd) return true;
  if (outputStart != cropStart + *writtenFrames) {
    setError(error, "Audio stem overlap-add produced a timeline gap.");
    return false;
  }
  const std::size_t offset = static_cast<std::size_t>(
      (outputStart - globalStart) * kChannels);
  const std::size_t frameCount =
      static_cast<std::size_t>(outputEnd - outputStart);
  for (std::size_t stem = 0; stem < kStemCount; ++stem) {
    if (!(*writers)[stem].writeFrames(overlap[stem].data() + offset,
                                      frameCount, error)) {
      return false;
    }
  }
  *writtenFrames += frameCount;
  return true;
}

void shiftOverlap(std::array<std::vector<float>, kStemCount>* overlap) {
  const std::size_t hopSamples =
      static_cast<std::size_t>(kChunkHopFrames) * kChannels;
  for (std::vector<float>& stem : *overlap) {
    std::move(stem.begin() + static_cast<std::ptrdiff_t>(hopSamples),
              stem.end(), stem.begin());
    std::fill(stem.end() - static_cast<std::ptrdiff_t>(hopSamples),
              stem.end(), 0.0f);
  }
}

}  // namespace

namespace {

bool separateMediaAudioUsingModel(
    const std::filesystem::path& mediaPath,
    const std::filesystem::path& modelPath,
    const ArtifactPaths& outputPaths,
    const ProgressCallback& onProgress,
    const DiagnosticReporter& diagnostics,
    const std::atomic<bool>* cancelRequested,
    std::string* error) {
  report(onProgress, 0.01f, "Loading DirectML separation model");
  BanditMaskModel model;
  std::string modelError;
  reportDiagnostic(diagnostics, DiagnosticLevel::Info, "model",
                   "Loading model: " + toUtf8String(modelPath));
  if (!model.initialize(modelPath, diagnostics, &modelError)) {
    setError(error, modelError + " Model: " + toUtf8String(modelPath));
    return false;
  }
  BanditSpectralTransform spectral;
  if (!spectral.initialize(error)) return false;
  report(onProgress, 0.03f, "DirectML GPU ready");

  const std::filesystem::path rawPath = temporaryRawAudioPathFor(mediaPath);
  ScopedTemporaryFile rawTemporary(rawPath);
  std::uint64_t sourceFrames = 0;
  if (!decodeToRawFile(mediaPath, rawPath, onProgress, cancelRequested,
                       &sourceFrames, error)) {
    return false;
  }

  RawAudioReader reader;
  if (!reader.open(rawPath, error)) return false;
  const ChunkPlan plan = buildChunkPlan(sourceFrames);
  const ArtifactPaths temporaryPaths = temporaryArtifactPathsFor(mediaPath);
  struct TemporaryArtifactCleanup {
    const ArtifactPaths& paths;
    bool published = false;
    ~TemporaryArtifactCleanup() {
      if (!published) removeArtifacts(paths);
    }
  } cleanup{temporaryPaths};

  std::array<audio_file::FlacWriter, kStemCount> writers;
  for (std::size_t stem = 0; stem < kStemCount; ++stem) {
    if (!writers[stem].open(temporaryPaths[stem], kSampleRate, kChannels,
                            error)) {
      return false;
    }
  }

  const std::vector<float> window = chunkWindow();
  std::vector<float> interleavedChunk;
  std::vector<float> monoChunk(static_cast<std::size_t>(kChunkFrames));
  std::vector<float> spectrogram;
  std::vector<float> masks;
  std::vector<float> reconstructed;
  std::array<std::vector<float>, kStemCount> overlap;
  for (std::vector<float>& stem : overlap) {
    stem.assign(static_cast<std::size_t>(kChunkFrames) * kChannels, 0.0f);
  }
  std::uint64_t writtenFrames = 0;

  for (std::uint64_t chunkIndex = 0; chunkIndex < plan.chunkCount;
       ++chunkIndex) {
    if (cancelled(cancelRequested)) {
      setError(error, "Audio separation cancelled.");
      return false;
    }
    const std::uint64_t chunkStart = chunkIndex * kChunkHopFrames;
    if (!reader.fillChunk(plan, chunkStart, &interleavedChunk, error)) {
      return false;
    }
    for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
      for (std::size_t frame = 0; frame < monoChunk.size(); ++frame) {
        monoChunk[frame] =
            interleavedChunk[frame * kChannels + channel];
      }
      if (!spectral.forward(monoChunk.data(), monoChunk.size(), &spectrogram,
                            error) ||
          !model.run(spectrogram, &masks, cancelRequested, error)) {
        return false;
      }
      for (std::size_t stem = 0; stem < kStemCount; ++stem) {
        const float* mask =
            masks.data() +
            stem * BanditSpectralTransform::kRealImagValues;
        if (!spectral.inverseMasked(spectrogram, mask, &reconstructed,
                                    error)) {
          return false;
        }
        for (std::size_t frame = 0; frame < reconstructed.size(); ++frame) {
          overlap[stem][frame * kChannels + channel] +=
              reconstructed[frame] * window[frame];
        }
      }
    }
    if (!writeCroppedHop(chunkStart, sourceFrames, overlap, &writers,
                         &writtenFrames, error)) {
      return false;
    }
    shiftOverlap(&overlap);
    const double completed =
        static_cast<double>(chunkIndex + 1) /
        static_cast<double>(std::max<std::uint64_t>(plan.chunkCount, 1));
    report(onProgress, static_cast<float>(0.07 + 0.89 * completed),
           "Separating dialogue, music and effects on GPU");
  }
  if (writtenFrames != sourceFrames) {
    setError(error, "Audio separation produced an incomplete timeline.");
    return false;
  }
  if (cancelled(cancelRequested)) {
    setError(error, "Audio separation cancelled.");
    return false;
  }
  report(onProgress, 0.97f, "Finalizing lossless audio stems");
  for (audio_file::FlacWriter& writer : writers) {
    if (!writer.finish(error)) return false;
  }
  if (!publishArtifactSet(temporaryPaths, outputPaths, error)) return false;
  cleanup.published = true;
  report(onProgress, 1.0f, "Audio stems ready");
  return true;
}

}  // namespace

bool separateMediaAudioWithModel(
    const std::filesystem::path& mediaPath,
    const std::filesystem::path& modelPath,
    const ArtifactPaths& outputPaths,
    const ProgressCallback& onProgress,
    const DiagnosticReporter& diagnostics,
    const std::atomic<bool>* cancelRequested,
    std::string* error) {
  if (error) error->clear();
  if (!validateRequest(mediaPath, cancelRequested, error) ||
      !validateModelPath(modelPath, error)) {
    return false;
  }
  return separateMediaAudioUsingModel(mediaPath, modelPath, outputPaths,
                                      onProgress, diagnostics,
                                      cancelRequested, error);
}

bool separateMediaAudio(const std::filesystem::path& mediaPath,
                        const ArtifactPaths& outputPaths,
                        const ProgressCallback& onProgress,
                        const DiagnosticReporter& diagnostics,
                        const std::atomic<bool>* cancelRequested,
                        std::string* error) {
  if (error) error->clear();
  if (!validateRequest(mediaPath, cancelRequested, error)) return false;

  std::filesystem::path modelPath;
  if (!resolveBundledModelPath(&modelPath, error)) return false;
  return separateMediaAudioUsingModel(mediaPath, modelPath, outputPaths,
                                      onProgress, diagnostics,
                                      cancelRequested, error);
}

}  // namespace audio_separation
