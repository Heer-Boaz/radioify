#include "playback/video/transcript/text_export.h"

#include <algorithm>
#include <fstream>
#include <utility>
#include <vector>

#include "core/file_output.h"
#include "playback/video/transcript/artifact.h"
#include "playback/video/transcript/document.h"
#include "runtime_helpers.h"

namespace playback_video_transcript {
namespace {

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

bool cancelled(
    const TextExportCancellationRequested& cancellationRequested) {
  return cancellationRequested && cancellationRequested();
}

void report(const TextExportProgress& reportProgress, float progress,
            std::string phase) {
  if (reportProgress) {
    reportProgress(std::clamp(progress, 0.0f, 1.0f), std::move(phase));
  }
}

}  // namespace

std::filesystem::path uniqueTextExportPathForVideo(
    const std::filesystem::path& videoPath) {
  return file_output::uniqueSiblingPath(videoPath, L" - transcript", L".txt");
}

bool exportTranscriptText(
    const std::filesystem::path& videoPath,
    const std::filesystem::path& outputPath,
    const TextExportProgress& reportProgress,
    const TextExportCancellationRequested& cancellationRequested,
    std::string* error) {
  if (error) error->clear();
  if (videoPath.empty() || outputPath.empty()) {
    setError(error, "The transcript export request is invalid.");
    return false;
  }
  if (cancelled(cancellationRequested)) return false;

  const std::filesystem::path transcriptPath =
      activeTranscriptPathForVideo(videoPath);
  if (transcriptPath.empty()) {
    setError(error, "Generate subtitles before exporting a transcript.");
    return false;
  }
  report(reportProgress, 0.05f, "Reading indexed transcript");
  std::vector<Segment> segments;
  if (!readIndexedTranscript(transcriptPath, &segments, error)) return false;

  std::optional<file_output::Transaction> transaction =
      file_output::Transaction::begin(outputPath,
                                      file_output::PublishMode::CreateNew,
                                      error);
  if (!transaction) return false;
  std::ofstream output(transaction->temporaryPath(),
                       std::ios::binary | std::ios::trunc);
  if (!output) {
    setError(error, "Could not create transcript export: " +
                        toUtf8String(outputPath.filename()));
    return false;
  }

  for (std::size_t index = 0; index < segments.size(); ++index) {
    if (cancelled(cancellationRequested)) return false;
    output << segments[index].text << "\r\n";
    if (!output) {
      setError(error, "Could not write the transcript export.");
      return false;
    }
    const float fraction = static_cast<float>(index + 1) /
                           static_cast<float>(segments.size());
    report(reportProgress, 0.1f + 0.85f * fraction,
           "Writing plain-text transcript");
  }
  output.flush();
  if (!output) {
    setError(error, "Could not finish the transcript export.");
    return false;
  }
  output.close();
  if (cancelled(cancellationRequested)) return false;
  report(reportProgress, 0.98f, "Publishing transcript export");
  if (!transaction->publish(error)) return false;
  report(reportProgress, 1.0f, "Transcript export ready");
  return true;
}

}  // namespace playback_video_transcript
