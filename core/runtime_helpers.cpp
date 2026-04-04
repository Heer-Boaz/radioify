#include "runtime_helpers.h"

#include <chrono>
#include <cstdlib>
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

#include "app_common.h"
#include "media_formats.h"

namespace {

std::string invalidSupportedAudioInputFileError(
    const std::filesystem::path& path) {
  if (path.empty()) return "Missing input file path.";
  if (!std::filesystem::exists(path)) {
    return "Input file not found: " + toUtf8String(path);
  }
  if (std::filesystem::is_directory(path)) {
    return "Input path must be a file: " + toUtf8String(path);
  }
  if (!isSupportedAudioExt(path)) {
    return "Unsupported input format '" + toUtf8String(path.extension()) +
           "'. Supported: " + supportedAudioExtensionsText() + ".";
  }
  return {};
}

std::filesystem::path currentPathOrDot() {
  std::error_code ec;
  std::filesystem::path dir = std::filesystem::current_path(ec);
  if (ec || dir.empty()) return ".";
  return dir;
}

std::string getEnvStringSafe(const char* name) {
  if (!name || name[0] == '\0') return {};
#ifdef _WIN32
  char* value = nullptr;
  size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || !value || length == 0) {
    std::free(value);
    return {};
  }
  std::string result(value);
  std::free(value);
  return result;
#else
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
#endif
}

std::filesystem::path executableDirOrEmpty() {
#ifdef _WIN32
  std::wstring buffer;
  DWORD size = MAX_PATH;
  for (;;) {
    buffer.resize(size);
    DWORD len = GetModuleFileNameW(nullptr, buffer.data(), size);
    if (len == 0) return {};
    if (len < size) {
      buffer.resize(len);
      return std::filesystem::path(buffer).parent_path();
    }
    size *= 2;
    if (size > 32768) return {};
  }
#else
  std::error_code ec;
  std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) return {};
  return exe.parent_path();
#endif
}

void appendUniquePath(std::vector<std::filesystem::path>* out,
                      const std::filesystem::path& path) {
  if (!out || path.empty()) return;
  for (const auto& existing : *out) {
    if (existing == path) return;
  }
  out->push_back(path);
}

std::filesystem::path firstCreatableDirectory(
    const std::vector<std::filesystem::path>& candidates) {
  for (const auto& candidate : candidates) {
    if (candidate.empty()) continue;
    std::error_code ec;
    std::filesystem::create_directories(candidate, ec);
    if (!ec && std::filesystem::is_directory(candidate, ec) && !ec) {
      return candidate;
    }
  }
  return {};
}

std::vector<std::filesystem::path> defaultWritableDataDirCandidates() {
  std::vector<std::filesystem::path> candidates;
#ifdef _WIN32
  const std::string localAppData = getEnvStringSafe("LOCALAPPDATA");
  if (!localAppData.empty()) {
    appendUniquePath(&candidates, std::filesystem::path(localAppData) / "Radioify");
  }
  const std::string appData = getEnvStringSafe("APPDATA");
  if (!appData.empty()) {
    appendUniquePath(&candidates, std::filesystem::path(appData) / "Radioify");
  }
#endif
  const std::filesystem::path exeDir = executableDirOrEmpty();
  appendUniquePath(&candidates, exeDir);
  appendUniquePath(&candidates, currentPathOrDot());
  return candidates;
}

}  // namespace

std::string toUtf8String(const std::filesystem::path& path) {
#ifdef _WIN32
  auto u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
#else
  return path.string();
#endif
}

int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::filesystem::path radioifyLaunchDir() {
  static const std::filesystem::path dir = currentPathOrDot();
  return dir;
}

std::filesystem::path radioifyExecutableDir() {
  static const std::filesystem::path dir = executableDirOrEmpty();
  return dir;
}

std::filesystem::path radioifyWritableDataDir() {
  static const std::filesystem::path dir =
      firstCreatableDirectory(defaultWritableDataDirCandidates());
  if (!dir.empty()) return dir;
  return radioifyLaunchDir();
}

std::filesystem::path radioifyLogPath() {
  return radioifyWritableDataDir() / "radioify.log";
}

std::vector<std::filesystem::path> radioifyResourceSearchRoots() {
  std::vector<std::filesystem::path> roots;
  const std::filesystem::path exeDir = radioifyExecutableDir();
  appendUniquePath(&roots, exeDir);
  if (!exeDir.empty()) {
    appendUniquePath(&roots, exeDir.parent_path());
  }
  appendUniquePath(&roots, radioifyLaunchDir());
  return roots;
}

bool validateSupportedAudioInputFile(const std::filesystem::path& path,
                                     std::string* error) {
  const std::string validationError = invalidSupportedAudioInputFileError(path);
  if (validationError.empty()) return true;
  if (error) *error = validationError;
  return false;
}

void requireSupportedAudioInputFile(const std::filesystem::path& path) {
  std::string error;
  if (!validateSupportedAudioInputFile(path, &error)) {
    die(error);
  }
}
