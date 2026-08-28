#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace audio_export {

using ProgressCallback = std::function<void(float, std::string)>;
using CancellationRequested = std::function<bool()>;
using OutputCommitStarted = std::function<bool()>;

bool supportsSource(const std::filesystem::path& sourceFile);
std::filesystem::path uniqueOutputPathFor(
    const std::filesystem::path& sourceFile);

// Decodes the best audio stream and writes a lossless FLAC using the native
// sample rate and channel count whenever FLAC can represent them. Output is
// published only after the complete stream has been finalized.
bool exportToFlac(const std::filesystem::path& sourceFile,
                  const std::filesystem::path& outputFile,
                  const ProgressCallback& reportProgress,
                  const CancellationRequested& cancellationRequested,
                  std::string* error,
                  const OutputCommitStarted& outputCommitStarted = {});

}  // namespace audio_export
