#pragma once

#include <filesystem>
#include <vector>

bool isKnownMediaArtworkSidecarPath(const std::filesystem::path& path);
void appendMediaArtworkSidecarCandidates(
    const std::filesystem::path& mediaPath,
    std::vector<std::filesystem::path>* outCandidates);
