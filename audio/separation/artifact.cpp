#include "audio/separation/artifact.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <system_error>
#include <utility>

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

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
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

ArtifactPaths temporaryArtifactPathsFor(
    const std::filesystem::path& mediaPath) {
  ArtifactPaths paths = artifactPathsFor(mediaPath);
  const std::string unique = uniquePart();
  for (std::filesystem::path& path : paths) {
    // The FLAC writer selects its muxer explicitly, so staging files do not
    // need a media extension. Keep the terminal extension non-media: browser
    // refreshes must never surface a partial stem as playable content.
    path += ".radioify-" + unique + ".tmp";
  }
  return paths;
}

std::filesystem::path temporaryRawAudioPathFor(
    const std::filesystem::path& mediaPath) {
  if (mediaPath.empty()) return {};
  std::filesystem::path filename = mediaPath.filename();
  filename += ".radioify-" + uniquePart() + ".tmp.f32";
  return mediaPath.parent_path() / filename;
}

void removeArtifacts(const ArtifactPaths& paths) {
  for (const std::filesystem::path& path : paths) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
}

bool publishArtifactSet(const ArtifactPaths& temporaryPaths,
                        const ArtifactPaths& finalPaths,
                        std::string* error) {
  if (error) error->clear();
  const std::string unique = uniquePart();
  ArtifactPaths backups{};
  std::array<bool, kStemCount> backupCreated{};
  std::array<bool, kStemCount> published{};

  for (std::size_t index = 0; index < kStemCount; ++index) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(temporaryPaths[index], ec) || ec) {
      setError(error, "A completed audio stem is missing: " +
                          temporaryPaths[index].filename().string());
      return false;
    }
  }

  auto rollBack = [&]() {
    for (std::size_t index = 0; index < kStemCount; ++index) {
      std::error_code ignored;
      if (published[index]) std::filesystem::remove(finalPaths[index], ignored);
    }
    for (std::size_t index = 0; index < kStemCount; ++index) {
      if (!backupCreated[index]) continue;
      std::error_code ignored;
      std::filesystem::rename(backups[index], finalPaths[index], ignored);
    }
  };

  for (std::size_t index = 0; index < kStemCount; ++index) {
    std::error_code ec;
    if (!std::filesystem::exists(finalPaths[index], ec) || ec) continue;
    backups[index] = finalPaths[index];
    backups[index] += ".radioify-" + unique + ".backup";
    std::filesystem::rename(finalPaths[index], backups[index], ec);
    if (ec) {
      rollBack();
      setError(error, "Could not preserve the existing audio stems: " +
                          ec.message());
      return false;
    }
    backupCreated[index] = true;
  }

  for (std::size_t index = 0; index < kStemCount; ++index) {
    std::error_code ec;
    std::filesystem::rename(temporaryPaths[index], finalPaths[index], ec);
    if (ec) {
      rollBack();
      setError(error, "Could not publish the separated audio stems: " +
                          ec.message());
      return false;
    }
    published[index] = true;
  }

  for (std::size_t index = 0; index < kStemCount; ++index) {
    if (!backupCreated[index]) continue;
    std::error_code ignored;
    std::filesystem::remove(backups[index], ignored);
  }
  return true;
}

}  // namespace audio_separation
