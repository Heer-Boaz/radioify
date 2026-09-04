#include "playback/video/transcript/artifact.h"

#include <algorithm>
#include <cstddef>
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

bool pathPrecedes(const std::filesystem::path &left,
                  const std::filesystem::path &right) {
  return lowercase(left.wstring()) < lowercase(right.wstring());
}

bool isLegacyNumberedTranscript(const std::filesystem::path &videoPath,
                                const std::filesystem::path &candidate) {
  const std::wstring prefix =
      lowercase(videoPath.stem().wstring()) + L".transcript.";
  const std::wstring stem = lowercase(candidate.stem().wstring());
  if (stem.rfind(prefix, 0) != 0 || stem.size() == prefix.size())
    return false;
  return std::all_of(stem.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
                     stem.end(),
                     [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
}

} // namespace

std::filesystem::path
transcriptPathForVideo(const std::filesystem::path &videoPath) {
  if (videoPath.filename().empty())
    return {};
  std::filesystem::path output = videoPath.parent_path() / videoPath.stem();
  output += ".transcript.srt";
  return output;
}

std::filesystem::path
languageTaggedTranscriptPathForVideo(const std::filesystem::path &videoPath,
                                     std::string_view language) {
  if (videoPath.filename().empty() || language.size() != 2 ||
      !std::all_of(language.begin(), language.end(),
                   [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; })) {
    return {};
  }
  std::filesystem::path output = videoPath.parent_path() / videoPath.stem();
  output += L".transcript." + std::wstring(language.begin(), language.end()) +
            L".srt";
  return output;
}

std::filesystem::path generatedEnglishTranscriptPathForVideo(
    const std::filesystem::path &videoPath) {
  if (videoPath.filename().empty())
    return {};
  std::filesystem::path output = videoPath.parent_path() / videoPath.filename();
  output += L".radioify.transcript.en.srt";
  return output;
}

bool isGeneratedEnglishTranscriptPath(
    const std::filesystem::path &videoPath,
    const std::filesystem::path &candidatePath) {
  if (videoPath.empty() || candidatePath.empty())
    return false;
  std::wstring expected = lowercase(
      generatedEnglishTranscriptPathForVideo(videoPath).lexically_normal().wstring());
  std::wstring candidate = lowercase(candidatePath.lexically_normal().wstring());
  return expected == candidate;
}

std::filesystem::path
activeTranscriptPathForVideo(const std::filesystem::path &videoPath) {
  const std::filesystem::path canonical = transcriptPathForVideo(videoPath);
  if (canonical.empty())
    return {};

  std::filesystem::path directory = videoPath.parent_path();
  if (directory.empty())
    directory = std::filesystem::path(L".");
  std::error_code ec;
  std::filesystem::directory_iterator entries(directory, ec);
  if (ec)
    return {};

  std::filesystem::path newest;
  std::filesystem::file_time_type newestTime{};
  bool haveNewest = false;
  bool newestIsLegacy = false;
  for (const auto &entry : entries) {
    ec.clear();
    if (!entry.is_regular_file(ec) || ec ||
        !playback_video_sidecars::isIndexedTranscriptSidecar(videoPath,
                                                             entry.path())) {
      continue;
    }

    ec.clear();
    const auto modified = entry.last_write_time(ec);
    if (ec)
      continue;
    const bool candidateIsLegacy =
        isLegacyNumberedTranscript(videoPath, entry.path());
    if (haveNewest && newestIsLegacy != candidateIsLegacy) {
      if (candidateIsLegacy)
        continue;
      newest = entry.path();
      newestTime = modified;
      newestIsLegacy = false;
      continue;
    }
    const bool preferCanonical = modified == newestTime &&
                                 entry.path() == canonical &&
                                 newest != canonical;
    if (!haveNewest || modified > newestTime || preferCanonical ||
        (modified == newestTime && !preferCanonical && newest != canonical &&
         pathPrecedes(entry.path(), newest))) {
      newest = entry.path();
      newestTime = modified;
      haveNewest = true;
      newestIsLegacy = candidateIsLegacy;
    }
  }
  return newest;
}

} // namespace playback_video_transcript
