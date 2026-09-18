#pragma once

#include <filesystem>

namespace playback_video_analysis {

// Root for private editing-analysis worker workspaces.
// RADIOIFY_ANALYSIS_CACHE_ROOT may select an absolute isolated root for
// diagnostics; normal analysis uses Radioify's writable data directory.
std::filesystem::path analysisCacheRoot();

} // namespace playback_video_analysis
