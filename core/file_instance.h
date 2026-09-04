#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

struct FileInstanceIdentity {
  std::uint64_t device = 0;
  std::uint64_t file = 0;
};

inline std::optional<FileInstanceIdentity> fileInstanceIdentity(
    const std::filesystem::path &path) {
#ifdef _WIN32
  const HANDLE handle = CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return std::nullopt;
  BY_HANDLE_FILE_INFORMATION information{};
  const bool succeeded =
      GetFileInformationByHandle(handle, &information) != FALSE;
  CloseHandle(handle);
  if (!succeeded)
    return std::nullopt;
  return FileInstanceIdentity{
      information.dwVolumeSerialNumber,
      (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32u) |
          information.nFileIndexLow};
#else
  struct stat information {};
  if (::stat(path.c_str(), &information) != 0)
    return std::nullopt;
  return FileInstanceIdentity{static_cast<std::uint64_t>(information.st_dev),
                              static_cast<std::uint64_t>(information.st_ino)};
#endif
}
