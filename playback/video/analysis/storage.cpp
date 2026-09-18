#include "playback/video/analysis/storage.h"

#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "core/runtime_helpers.h"

namespace playback_video_analysis {
namespace {

std::optional<std::filesystem::path> isolatedCacheRoot() {
#ifdef _WIN32
  constexpr wchar_t kName[] = L"RADIOIFY_ANALYSIS_CACHE_ROOT";
  const DWORD required = GetEnvironmentVariableW(kName, nullptr, 0);
  if (required == 0)
    return std::nullopt;
  std::vector<wchar_t> value(required);
  const DWORD copied = GetEnvironmentVariableW(kName, value.data(), required);
  if (copied == 0 || copied >= required)
    return std::filesystem::path{};
  value.resize(copied);
  return std::filesystem::path(std::wstring(value.begin(), value.end()));
#else
  if (const std::optional<std::string> value =
          getEnvString("RADIOIFY_ANALYSIS_CACHE_ROOT")) {
    return pathFromUtf8String(*value);
  }
  return std::nullopt;
#endif
}

} // namespace

std::filesystem::path analysisCacheRoot() {
  static const std::filesystem::path root = [] {
    if (const std::optional<std::filesystem::path> isolated =
            isolatedCacheRoot()) {
      std::filesystem::path path = *isolated;
      if (!path.empty() && path.is_absolute())
        return path.lexically_normal();
      return std::filesystem::path{};
    }
    return radioifyWritableDataDir() / "cache" / "video-analysis";
  }();
  return root;
}

} // namespace playback_video_analysis
