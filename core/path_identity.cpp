#include "core/path_identity.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <limits>
#include <system_error>
#include <utility>
#include <vector>

namespace {

std::filesystem::path trimTrailingSeparator(std::filesystem::path path) {
  if (path != path.root_path() && path.filename().empty()) {
    path = path.parent_path();
  }
  return path;
}

#ifdef _WIN32
std::filesystem::path fullWindowsPath(const std::filesystem::path& path) {
  const std::wstring input = path.native();
  const DWORD required = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
  if (required == 0) {
    return {};
  }

  std::vector<wchar_t> buffer(static_cast<size_t>(required));
  const DWORD written =
      GetFullPathNameW(input.c_str(), required, buffer.data(), nullptr);
  if (written == 0 || written >= required) {
    return {};
  }

  std::wstring normalized(buffer.data(), written);
  constexpr wchar_t kExtendedUncPrefix[] = L"\\\\?\\UNC\\";
  constexpr wchar_t kExtendedPrefix[] = L"\\\\?\\";
  if (normalized.rfind(kExtendedUncPrefix, 0) == 0) {
    normalized = L"\\\\" + normalized.substr(8);
  } else if (normalized.rfind(kExtendedPrefix, 0) == 0) {
    normalized.erase(0, 4);
  }
  return std::filesystem::path(std::move(normalized));
}
#endif

}  // namespace

PathIdentity makePathIdentity(const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }

  std::filesystem::path normalized;
#ifdef _WIN32
  normalized = fullWindowsPath(path);
#endif
  if (normalized.empty()) {
    std::error_code ec;
    normalized = std::filesystem::absolute(path, ec);
    if (ec || normalized.empty()) {
      normalized = path;
    }
  }
  normalized = trimTrailingSeparator(normalized.lexically_normal());
  return {std::move(normalized)};
}

bool samePathIdentity(const PathIdentity& left, const PathIdentity& right) {
  if (left.empty() || right.empty()) {
    return left.empty() && right.empty();
  }
#ifdef _WIN32
  const std::wstring& leftText = left.normalizedPath.native();
  const std::wstring& rightText = right.normalizedPath.native();
  if (leftText.size() <=
          static_cast<size_t>((std::numeric_limits<int>::max)()) &&
      rightText.size() <=
          static_cast<size_t>((std::numeric_limits<int>::max)())) {
    const int result = CompareStringOrdinal(
        leftText.data(), static_cast<int>(leftText.size()), rightText.data(),
        static_cast<int>(rightText.size()), TRUE);
    if (result != 0) {
      return result == CSTR_EQUAL;
    }
  }
#endif
  return left.normalizedPath == right.normalizedPath;
}

std::filesystem::path pathIdentityKey(const PathIdentity& identity) {
  if (identity.empty()) {
    return {};
  }
#ifdef _WIN32
  const std::wstring& input = identity.normalizedPath.native();
  if (input.size() <= static_cast<size_t>((std::numeric_limits<int>::max)())) {
    const int required = LCMapStringEx(
        LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, input.data(),
        static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr, 0);
    if (required > 0) {
      std::wstring mapped(static_cast<size_t>(required), L'\0');
      const int written = LCMapStringEx(
          LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, input.data(),
          static_cast<int>(input.size()), mapped.data(), required, nullptr,
          nullptr, 0);
      if (written == required) {
        return std::filesystem::path(std::move(mapped));
      }
    }
  }
#endif
  return identity.normalizedPath;
}

bool samePath(const std::filesystem::path& left,
              const std::filesystem::path& right) {
  if (left == right) {
    return true;
  }
  if (left.empty() || right.empty()) {
    return false;
  }
  return makePathIdentity(left) == makePathIdentity(right);
}
