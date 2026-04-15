#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

enum class MediaArtworkSidecarPolicy : uint8_t {
  IncludeGenericDirectoryFallback,
  FileSpecificOnly,
};

bool isKnownMediaArtworkSidecarPath(const std::filesystem::path& path);
void appendMediaArtworkSidecarCandidates(
    const std::filesystem::path& mediaPath,
    MediaArtworkSidecarPolicy policy,
    std::vector<std::filesystem::path>* outCandidates);
