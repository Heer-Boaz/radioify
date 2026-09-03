#pragma once

#include <filesystem>

namespace playback_video_chapters {

// Root shared by durable chapter documents and private worker checkpoints.
// RADIOIFY_CHAPTER_CACHE_ROOT may select an absolute isolated root for
// diagnostics; normal playback uses Radioify's writable data directory.
std::filesystem::path analysisCacheRoot();

} // namespace playback_video_chapters
