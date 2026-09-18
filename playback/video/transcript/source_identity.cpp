#include "playback/video/transcript/source_identity.h"

namespace playback_video_transcript {

std::optional<TranscriptSourceIdentity> captureTranscriptSourceIdentity(
    const std::filesystem::path& path, std::string* error) {
  if (error) error->clear();
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (!ec) {
    const auto modified = std::filesystem::last_write_time(path, ec);
    if (!ec) {
      if (const auto instance = fileInstanceIdentity(path))
        return TranscriptSourceIdentity{size, modified, *instance};
    }
  }
  if (error) *error = "Could not identify the source video.";
  return std::nullopt;
}

bool transcriptSourceMatches(const TranscriptSourceIdentity& expected,
                             const std::filesystem::path& path) {
  const auto current = captureTranscriptSourceIdentity(path);
  return current && current->size == expected.size && current->modified == expected.modified &&
         current->instance.device == expected.instance.device &&
         current->instance.file == expected.instance.file;
}

}  // namespace playback_video_transcript
