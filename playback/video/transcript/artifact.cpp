#include "playback/video/transcript/artifact.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <system_error>

#include "playback/video/sidecar_identity.h"

namespace playback_video_transcript {
namespace {

std::wstring lowercase(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return value;
}

bool pathPrecedes(const std::filesystem::path& left,
                  const std::filesystem::path& right) {
  return lowercase(left.wstring()) < lowercase(right.wstring());
}

}  // namespace

std::filesystem::path transcriptPathForVideo(
    const std::filesystem::path& videoPath) {
  if (videoPath.filename().empty()) return {};
  std::filesystem::path output = videoPath.parent_path() / videoPath.stem();
  output += ".transcript.srt";
  return output;
}

std::filesystem::path activeTranscriptPathForVideo(
    const std::filesystem::path& videoPath) {
  const std::filesystem::path canonical = transcriptPathForVideo(videoPath);
  if (canonical.empty()) return {};

  std::error_code ec;
  if (std::filesystem::is_regular_file(canonical, ec) && !ec) {
    return canonical;
  }

  std::filesystem::path directory = videoPath.parent_path();
  if (directory.empty()) directory = std::filesystem::path(L".");
  std::filesystem::directory_iterator entries(directory, ec);
  if (ec) return {};

  std::filesystem::path newest;
  std::filesystem::file_time_type newestTime{};
  bool haveNewest = false;
  for (const auto& entry : entries) {
    ec.clear();
    if (!entry.is_regular_file(ec) || ec ||
        !playback_video_sidecars::isIndexedTranscriptSidecar(
            videoPath, entry.path())) {
      continue;
    }

    ec.clear();
    const auto modified = entry.last_write_time(ec);
    if (ec) continue;
    if (!haveNewest || modified > newestTime ||
        (modified == newestTime && pathPrecedes(entry.path(), newest))) {
      newest = entry.path();
      newestTime = modified;
      haveNewest = true;
    }
  }
  return newest;
}

}  // namespace playback_video_transcript
