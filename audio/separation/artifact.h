#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace audio_separation {

enum class Stem : std::uint8_t {
  Dialogue = 0,
  Music = 1,
  Effects = 2,
};

constexpr std::size_t kStemCount = 3;
using ArtifactPaths = std::array<std::filesystem::path, kStemCount>;

const char* stemFileSuffix(Stem stem);
const char* stemDisplayName(Stem stem);
ArtifactPaths artifactPathsFor(const std::filesystem::path& mediaPath);
bool artifactsExistFor(const std::filesystem::path& mediaPath);
bool isManagedArtifactPath(const std::filesystem::path& path);
std::filesystem::path temporaryRawAudioPathFor(
    const std::filesystem::path& mediaPath);

}  // namespace audio_separation
