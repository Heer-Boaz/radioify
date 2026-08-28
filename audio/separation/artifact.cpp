#include "audio/separation/artifact.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace audio_separation {
namespace {

std::atomic<std::uint64_t> gTemporarySequence{0};

std::uint64_t processId() {
#ifdef _WIN32
  return static_cast<std::uint64_t>(_getpid());
#else
  return static_cast<std::uint64_t>(getpid());
#endif
}

std::string uniquePart() {
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::uint64_t sequence =
      gTemporarySequence.fetch_add(1, std::memory_order_relaxed);
  return std::to_string(processId()) + "-" + std::to_string(ticks) + "-" +
         std::to_string(sequence);
}

std::filesystem::path stemPath(const std::filesystem::path& mediaPath,
                               const char* suffix) {
  if (mediaPath.empty()) return {};
  std::filesystem::path result = mediaPath;
  result.replace_extension();
  result += suffix;
  result += ".flac";
  return result;
}

}  // namespace

const char* stemFileSuffix(Stem stem) {
  switch (stem) {
    case Stem::Dialogue:
      return ".dialogue";
    case Stem::Music:
      return ".music";
    case Stem::Effects:
      return ".effects";
  }
  return ".unknown";
}

const char* stemDisplayName(Stem stem) {
  switch (stem) {
    case Stem::Dialogue:
      return "Dialogue";
    case Stem::Music:
      return "Music";
    case Stem::Effects:
      return "Sound effects";
  }
  return "Unknown";
}

ArtifactPaths artifactPathsFor(const std::filesystem::path& mediaPath) {
  return {stemPath(mediaPath, stemFileSuffix(Stem::Dialogue)),
          stemPath(mediaPath, stemFileSuffix(Stem::Music)),
          stemPath(mediaPath, stemFileSuffix(Stem::Effects))};
}

bool artifactsExistFor(const std::filesystem::path& mediaPath) {
  const ArtifactPaths paths = artifactPathsFor(mediaPath);
  return std::all_of(paths.begin(), paths.end(),
                     [](const std::filesystem::path& path) {
                       std::error_code ec;
                       return std::filesystem::is_regular_file(path, ec) &&
                              !ec;
                     });
}

bool isManagedArtifactPath(const std::filesystem::path& path) {
  auto lowerExtension = [](const std::filesystem::path& value) {
    std::string extension = value.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) {
                     return static_cast<char>(std::tolower(character));
                   });
    return extension;
  };
  if (lowerExtension(path) != ".flac") return false;
  const std::string stemExtension = lowerExtension(path.stem());
  return stemExtension == stemFileSuffix(Stem::Dialogue) ||
         stemExtension == stemFileSuffix(Stem::Music) ||
         stemExtension == stemFileSuffix(Stem::Effects);
}

std::filesystem::path temporaryRawAudioPathFor(
    const std::filesystem::path& mediaPath) {
  if (mediaPath.empty()) return {};
  std::filesystem::path filename = mediaPath.filename();
  filename += ".radioify-" + uniquePart() + ".tmp.f32";
  return mediaPath.parent_path() / filename;
}

}  // namespace audio_separation
