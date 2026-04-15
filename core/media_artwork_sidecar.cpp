#include "media_artwork_sidecar.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

#include "runtime_helpers.h"

namespace {

std::string lowercaseAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

std::string lowercasePathStemUtf8(const std::filesystem::path& path) {
  return lowercaseAscii(toUtf8String(path.stem()));
}

bool isKnownArtworkBaseName(const std::string& stemLower) {
  static constexpr std::array<const char*, 7> kKnownNames = {
      "cover",      "folder",     "front",      "album",
      "albumart",   "albumartsmall", "albumartlarge"};
  return std::find_if(kKnownNames.begin(), kKnownNames.end(),
                      [&](const char* candidate) {
                        return stemLower == candidate;
                      }) != kKnownNames.end();
}

}  // namespace

bool isKnownMediaArtworkSidecarPath(const std::filesystem::path& path) {
  return isKnownArtworkBaseName(lowercasePathStemUtf8(path));
}

void appendMediaArtworkSidecarCandidates(
    const std::filesystem::path& mediaPath,
    std::vector<std::filesystem::path>* outCandidates) {
  if (!outCandidates) {
    return;
  }

  const std::filesystem::path dir = mediaPath.parent_path();
  if (dir.empty()) {
    return;
  }

  const std::string stemLower = lowercasePathStemUtf8(mediaPath);
  static constexpr std::array<const char*, 4> kExts = {
      ".jpg", ".jpeg", ".png", ".bmp"};
  static constexpr std::array<const char*, 7> kNames = {
      "cover",      "folder",     "front",      "album",
      "albumart",   "albumartsmall", "albumartlarge"};

  outCandidates->reserve(outCandidates->size() +
                         kExts.size() * (kNames.size() + 1));
  for (const char* name : kNames) {
    for (const char* ext : kExts) {
      outCandidates->push_back(dir / (std::string(name) + ext));
    }
  }
  if (!stemLower.empty()) {
    for (const char* ext : kExts) {
      outCandidates->push_back(dir / (stemLower + ext));
    }
  }
}
