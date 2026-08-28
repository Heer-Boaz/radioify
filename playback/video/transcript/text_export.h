#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace playback_video_transcript {

using TextExportProgress = std::function<void(float, std::string)>;
using TextExportCancellationRequested = std::function<bool()>;
using TextExportCommitStarted = std::function<bool()>;

std::filesystem::path uniqueTextExportPathForVideo(
    const std::filesystem::path& videoPath);

// Exports the active managed transcript as plain UTF-8 text. The destination
// deliberately does not use a subtitle-sidecar name, so playback cannot
// rediscover an exported document as a second subtitle track.
bool exportTranscriptText(
    const std::filesystem::path& videoPath,
    const std::filesystem::path& outputPath,
    const TextExportProgress& reportProgress,
    const TextExportCancellationRequested& cancellationRequested,
    std::string* error,
    const TextExportCommitStarted& outputCommitStarted = {});

}  // namespace playback_video_transcript
