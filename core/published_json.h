#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

// Progress producers publish by atomic replacement. std::ifstream on Windows
// denies deletion of its open file and can make that publication fail.
inline nlohmann::json readPublishedJson(const std::filesystem::path &path,
                                        std::uintmax_t maxBytes = 65536) {
  std::string contents;
  {
    const HANDLE file =
        CreateFileW(path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      return {};
    struct CloseFile {
      HANDLE file;
      ~CloseFile() { CloseHandle(file); }
    } close{file};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<std::uintmax_t>(size.QuadPart) > maxBytes ||
        size.QuadPart > MAXDWORD)
      return {};
    contents.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD received = 0;
    if (!ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()),
                  &received, nullptr) ||
        received != contents.size())
      return {};
  }
  return nlohmann::json::parse(contents, nullptr, false);
}
