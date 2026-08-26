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
ArtifactPaths temporaryArtifactPathsFor(
    const std::filesystem::path& mediaPath);
std::filesystem::path temporaryRawAudioPathFor(
    const std::filesystem::path& mediaPath);

// Publishes all stems as one managed artifact set. Existing stems are kept
// until every temporary output is complete; a failed transaction restores the
// previous set.
bool publishArtifactSet(const ArtifactPaths& temporaryPaths,
                        const ArtifactPaths& finalPaths,
                        std::string* error);
void removeArtifacts(const ArtifactPaths& paths);

}  // namespace audio_separation
