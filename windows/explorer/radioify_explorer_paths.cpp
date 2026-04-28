#include "radioify_explorer_paths.h"

#include <cwctype>

namespace {

std::wstring lowercasePathText(std::wstring value) {
  for (wchar_t& ch : value) {
    ch = static_cast<wchar_t>(std::towlower(ch));
  }
  return value;
}

}  // namespace

std::wstring parentPathOf(std::wstring_view path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring_view::npos) return {};
  return std::wstring(path.substr(0, slash));
}

std::wstring joinPath(std::wstring_view parent, std::wstring_view child) {
  if (parent.empty()) return std::wstring(child);
  std::wstring result(parent);
  if (result.back() != L'\\' && result.back() != L'/') result.push_back(L'\\');
  result.append(child);
  return result;
}

bool isExistingFile(const std::wstring& path) noexcept {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring extensionOfPath(std::wstring_view path) {
  const size_t slash = path.find_last_of(L"\\/");
  const size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring_view::npos ||
      (slash != std::wstring_view::npos && dot < slash)) {
    return {};
  }
  return lowercasePathText(std::wstring(path.substr(dot)));
}

std::wstring filenameStemOfPath(std::wstring_view path) {
  const size_t slash = path.find_last_of(L"\\/");
  const size_t begin = slash == std::wstring_view::npos ? 0 : slash + 1;
  const size_t dot = path.find_last_of(L'.');
  const size_t end =
      (dot == std::wstring_view::npos || dot < begin) ? path.size() : dot;
  return std::wstring(path.substr(begin, end - begin));
}
