#include "playback/video/subtitle/sidecar_discovery.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <system_error>

#include "playback/video/sidecar_identity.h"

namespace playback_video_subtitle {
namespace {

std::wstring lowercase(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return value;
}

bool isSubtitleDirectoryName(const std::filesystem::path& path) {
  const std::wstring name = lowercase(path.filename().wstring());
  return name == L"subs" || name == L"sub" || name == L"subtitle" ||
         name == L"subtitles";
}

void appendDirectoryCandidates(
    const std::filesystem::path& videoPath,
    const std::filesystem::path& directory, bool recursive, int maxDepth,
    std::vector<std::filesystem::path>* candidates) {
  if (!candidates) return;
  std::error_code ec;
  if (!recursive) {
    for (std::filesystem::directory_iterator it(directory, ec), end; it != end;
         it.increment(ec)) {
      if (ec) break;
      if (it->is_regular_file(ec) && !ec &&
          isAutomaticSubtitleSidecar(videoPath, it->path())) {
        candidates->push_back(it->path());
      }
      ec.clear();
    }
    return;
  }

  const auto options =
      std::filesystem::directory_options::skip_permission_denied;
  for (std::filesystem::recursive_directory_iterator it(directory, options, ec),
       end;
       it != end; it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    if (it->is_directory(ec)) {
      if (!ec && it.depth() >= maxDepth) it.disable_recursion_pending();
      ec.clear();
      continue;
    }
    if (it->is_regular_file(ec) && !ec &&
        isAutomaticSubtitleSidecar(videoPath, it->path())) {
      candidates->push_back(it->path());
    }
    ec.clear();
  }
}

}  // namespace

bool isAutomaticSubtitleSidecar(
    const std::filesystem::path& videoPath,
    const std::filesystem::path& candidatePath) {
  return playback_video_sidecars::classifyAutomaticSubtitleSidecar(
             videoPath, candidatePath) !=
         playback_video_sidecars::AutomaticSubtitleKind::None;
}

std::vector<std::filesystem::path> discoverAutomaticSubtitleSidecars(
    const std::filesystem::path& videoPath) {
  std::vector<std::filesystem::path> candidates;
  std::filesystem::path directory = videoPath.parent_path();
  if (directory.empty()) directory = std::filesystem::path(L".");

  std::error_code ec;
  if (!std::filesystem::is_directory(directory, ec) || ec) return candidates;
  appendDirectoryCandidates(videoPath, directory, false, 0, &candidates);

  for (std::filesystem::directory_iterator it(directory, ec), end; it != end;
       it.increment(ec)) {
    if (ec) break;
    if (it->is_directory(ec) && !ec &&
        isSubtitleDirectoryName(it->path())) {
      appendDirectoryCandidates(videoPath, it->path(), true, 2, &candidates);
    }
    ec.clear();
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const auto& left, const auto& right) {
              return lowercase(left.wstring()) < lowercase(right.wstring());
            });
  candidates.erase(
      std::unique(candidates.begin(), candidates.end(),
                  [](const auto& left, const auto& right) {
                    return lowercase(left.wstring()) ==
                           lowercase(right.wstring());
                  }),
      candidates.end());
  return candidates;
}

}  // namespace playback_video_subtitle
