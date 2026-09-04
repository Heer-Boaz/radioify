#include "playback/video/sidecar_identity.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace playback_video_sidecars {
namespace {

std::wstring lowercase(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return value;
}

bool isSubtitleExtension(std::wstring_view extension) {
  return extension == L".srt" || extension == L".vtt" || extension == L".ass" ||
         extension == L".ssa" || extension == L".sbv" || extension == L".sub" ||
         extension == L".txt" || extension == L".smi" || extension == L".sami";
}

bool isAsciiAlpha(wchar_t ch) {
  return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
}

bool isLanguageQualifier(std::wstring_view value) {
  if (value == L"english" || value == L"dutch" || value == L"japanese") {
    return true;
  }
  if (value.size() == 2 || value.size() == 3) {
    return std::all_of(value.begin(), value.end(), isAsciiAlpha);
  }

  // Accept common BCP-47 shapes such as en-US and zh-Hans. The primary
  // language remains deliberately restricted to the usual 2/3-letter form.
  const size_t separator = value.find(L'-');
  if (separator != 2 && separator != 3)
    return false;
  if (!std::all_of(value.begin(), value.begin() + separator, isAsciiAlpha)) {
    return false;
  }
  const std::wstring_view remainder = value.substr(separator + 1);
  return (remainder.size() == 2 || remainder.size() == 3 ||
          remainder.size() == 4) &&
         std::all_of(remainder.begin(), remainder.end(), isAsciiAlpha);
}

bool isRoleQualifier(std::wstring_view value) {
  return value == L"forced" || value == L"sdh" || value == L"cc" ||
         value == L"hi" || value == L"commentary" || value == L"signs" ||
         value == L"songs" || value == L"default";
}

bool isDecimal(std::wstring_view value) {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(),
                     [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
}

std::vector<std::wstring_view> splitQualifiers(std::wstring_view value) {
  std::vector<std::wstring_view> parts;
  while (!value.empty()) {
    const size_t separator = value.find(L'.');
    const std::wstring_view part = value.substr(0, separator);
    if (part.empty())
      return {};
    parts.push_back(part);
    if (separator == std::wstring_view::npos)
      break;
    value.remove_prefix(separator + 1);
  }
  return parts;
}

AutomaticSubtitleKind classifyQualifier(std::wstring_view suffix,
                                        std::wstring_view extension) {
  if (suffix.empty() || suffix.front() != L'.') {
    return AutomaticSubtitleKind::None;
  }
  suffix.remove_prefix(1);
  const std::vector<std::wstring_view> parts = splitQualifiers(suffix);
  if (parts.empty())
    return AutomaticSubtitleKind::None;

  if (parts.front() == L"transcript") {
    const bool validIndex =
        parts.size() == 1 ||
        (parts.size() == 2 &&
         (isDecimal(parts[1]) || isLanguageQualifier(parts[1])));
    if (!validIndex)
      return AutomaticSubtitleKind::None;
    return extension == L".srt" ? AutomaticSubtitleKind::IndexedTranscript
                                : AutomaticSubtitleKind::Subtitle;
  }

  if (!isLanguageQualifier(parts.front()) && !isRoleQualifier(parts.front())) {
    return AutomaticSubtitleKind::None;
  }
  return std::all_of(parts.begin() + 1, parts.end(), isRoleQualifier)
             ? AutomaticSubtitleKind::Subtitle
             : AutomaticSubtitleKind::None;
}

} // namespace

AutomaticSubtitleKind
classifyAutomaticSubtitleSidecar(const std::filesystem::path &videoPath,
                                 const std::filesystem::path &candidatePath) {
  if (videoPath.filename().empty())
    return AutomaticSubtitleKind::None;

  const std::wstring extension = lowercase(candidatePath.extension().wstring());
  if (!isSubtitleExtension(extension))
    return AutomaticSubtitleKind::None;

  const std::wstring videoStem = lowercase(videoPath.stem().wstring());
  const std::wstring candidateStem = lowercase(candidatePath.stem().wstring());
  if (videoStem.empty() || candidateStem.size() < videoStem.size() ||
      candidateStem.compare(0, videoStem.size(), videoStem) != 0) {
    return AutomaticSubtitleKind::None;
  }
  if (candidateStem.size() == videoStem.size()) {
    return AutomaticSubtitleKind::Subtitle;
  }
  return classifyQualifier(
      std::wstring_view(candidateStem).substr(videoStem.size()), extension);
}

bool isIndexedTranscriptSidecar(const std::filesystem::path &videoPath,
                                const std::filesystem::path &candidatePath) {
  return classifyAutomaticSubtitleSidecar(videoPath, candidatePath) ==
         AutomaticSubtitleKind::IndexedTranscript;
}

} // namespace playback_video_sidecars
