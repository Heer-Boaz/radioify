#include "audio/audio_export.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "audio/ffmpegaudio.h"
#include "audio/flac_writer.h"
#include "audio/media_formats.h"
#include "core/file_output.h"

namespace audio_export {
namespace {

constexpr std::uint32_t kDecodeBlockFrames = 8192;
constexpr std::uint32_t kMaximumFlacChannels = 8;
constexpr std::uint32_t kMaximumFlacSampleRate = 655350;

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

bool cancelled(const CancellationRequested& cancellationRequested) {
  return cancellationRequested && cancellationRequested();
}

void report(const ProgressCallback& reportProgress, float progress,
            std::string phase) {
  if (reportProgress) {
    reportProgress(std::clamp(progress, 0.0f, 1.0f), std::move(phase));
  }
}

}  // namespace

bool supportsSource(const std::filesystem::path& sourceFile) {
  return isSupportedVideoExt(sourceFile) || isMiniaudioExt(sourceFile) ||
         isFfmpegAudioExt(sourceFile) || isM4aExt(sourceFile);
}

std::filesystem::path uniqueOutputPathFor(
    const std::filesystem::path& sourceFile) {
  return file_output::uniqueSiblingPath(sourceFile, L" - audio", L".flac");
}

bool exportToFlac(const std::filesystem::path& sourceFile,
                  const std::filesystem::path& outputFile,
                  const ProgressCallback& reportProgress,
                  const CancellationRequested& cancellationRequested,
                  std::string* error,
                  const OutputCommitStarted& outputCommitStarted) {
  if (error) error->clear();
  if (!supportsSource(sourceFile) || outputFile.empty()) {
    setError(error, "The audio export request is invalid.");
    return false;
  }
  if (cancelled(cancellationRequested)) return false;

  report(reportProgress, 0.01f, "Inspecting source audio");
  FfmpegAudioStreamFormat sourceFormat;
  if (!probeFfmpegAudioStream(sourceFile, &sourceFormat, error)) return false;
  const std::uint32_t channels =
      sourceFormat.channels <= kMaximumFlacChannels ? sourceFormat.channels
                                                    : 2u;
  const std::uint32_t sampleRate =
      sourceFormat.sampleRate <= kMaximumFlacSampleRate
          ? sourceFormat.sampleRate
          : 48000u;

  std::optional<file_output::Transaction> transaction =
      file_output::Transaction::begin(outputFile,
                                      file_output::PublishMode::CreateNew,
                                      error);
  if (!transaction) return false;

  FfmpegAudioDecoder decoder;
  if (!decoder.init(sourceFile, channels, sampleRate, error)) return false;
  std::uint64_t expectedFrames = 0;
  decoder.getTotalFrames(&expectedFrames);
  std::uint64_t initialPadding = 0;
  std::uint64_t trailingPadding = 0;
  decoder.getPaddingFrames(&initialPadding, &trailingPadding);

  audio_file::FlacWriter writer;
  if (!writer.open(transaction->temporaryPath(), sampleRate, channels,
                   error)) {
    return false;
  }

  report(reportProgress, 0.03f, "Extracting lossless audio");
  std::vector<float> decoded(
      static_cast<std::size_t>(kDecodeBlockFrames) * channels);
  std::vector<float> trailing;
  std::uint64_t decodedFrames = 0;
  std::uint64_t skippedInitialFrames = 0;
  std::uint64_t writtenFrames = 0;
  for (;;) {
    if (cancelled(cancellationRequested)) return false;
    std::uint64_t framesRead = 0;
    if (!decoder.readFrames(decoded.data(), kDecodeBlockFrames,
                            &framesRead)) {
      setError(error, "Could not decode the source audio.");
      return false;
    }
    if (framesRead == 0) break;
    decodedFrames += framesRead;

    const std::uint64_t remainingInitial =
        initialPadding > skippedInitialFrames
            ? initialPadding - skippedInitialFrames
            : 0;
    const std::uint64_t skip = std::min(framesRead, remainingInitial);
    skippedInitialFrames += skip;
    const std::uint64_t usableFrames = framesRead - skip;
    if (usableFrames > 0) {
      const float* usable = decoded.data() + skip * channels;
      trailing.insert(trailing.end(), usable,
                      usable + usableFrames * channels);
      const std::uint64_t bufferedFrames =
          static_cast<std::uint64_t>(trailing.size() / channels);
      if (bufferedFrames > trailingPadding) {
        const std::uint64_t readyFrames = bufferedFrames - trailingPadding;
        if (!writer.writeFrames(trailing.data(),
                                static_cast<std::size_t>(readyFrames),
                                error)) {
          return false;
        }
        writtenFrames += readyFrames;
        trailing.erase(
            trailing.begin(),
            trailing.begin() + static_cast<std::ptrdiff_t>(readyFrames *
                                                            channels));
      }
    }

    if (expectedFrames > 0) {
      const double rawExpected = static_cast<double>(
          expectedFrames + initialPadding + trailingPadding);
      const float fraction = static_cast<float>(
          static_cast<double>(decodedFrames) / std::max(1.0, rawExpected));
      report(reportProgress, 0.03f + 0.93f * std::min(fraction, 1.0f),
             "Extracting lossless audio");
    }
  }
  if (cancelled(cancellationRequested)) return false;
  if (writtenFrames == 0) {
    setError(error, "The source contains no exportable audio frames.");
    return false;
  }

  report(reportProgress, 0.97f, "Finalizing FLAC audio");
  if (!writer.finish(error)) return false;
  if (cancelled(cancellationRequested)) return false;
  if (outputCommitStarted && !outputCommitStarted()) return false;
  if (!transaction->publish(error)) return false;
  report(reportProgress, 1.0f, "Audio export ready");
  return true;
}

}  // namespace audio_export
