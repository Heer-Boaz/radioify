#include "runtime_helpers.h"

#include <chrono>

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
