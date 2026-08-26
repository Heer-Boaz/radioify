#pragma once

#include <filesystem>
#include <vector>

namespace playback_video_subtitle {

// Automatic subtitle discovery is intentionally exact. A sidecar must belong
// to the complete video stem and may only add a recognized language/role or a
// Radioify transcript qualifier. Arbitrary files from the same directory are
// never associated implicitly.
bool isAutomaticSubtitleSidecar(const std::filesystem::path& videoPath,
                                const std::filesystem::path& candidatePath);

std::vector<std::filesystem::path> discoverAutomaticSubtitleSidecars(
    const std::filesystem::path& videoPath);

}  // namespace playback_video_subtitle
