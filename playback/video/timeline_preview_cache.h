#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "playback/video/timeline_preview_types.h"

namespace playback_video_timeline_preview {

struct CacheConfig {
  size_t memoryByteLimit = kMaxCacheBytes;
  size_t memoryFrameLimit = kMaxCacheFrames;
  uint64_t persistentByteLimit = 512ull * 1024ull * 1024ull;
  // Empty selects Radioify's versioned cache directory.
  std::filesystem::path persistentRoot;
};

struct CacheLookup {
  std::shared_ptr<const Image> image;
  ResultOrigin origin = ResultOrigin::MemoryCache;
};

// Worker-owned two-tier thumbnail cache.  Entries are exact timestamp/profile
// matches: quantization belongs to HoverModel, never to cache lookup.
class Cache {
 public:
  explicit Cache(CacheConfig config = {});
  ~Cache();

  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;

  bool open(const Source& source, std::string* error = nullptr);
  void close();

  CacheLookup find(int64_t targetUs, uint64_t imageId);
  void storeMemory(const std::shared_ptr<const Image>& image);
  void storePersistent(const std::shared_ptr<const Image>& image);
  void prunePersistent();

  size_t memoryBytes() const;
  size_t memoryFrames() const;
  std::filesystem::path persistentDirectory() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_timeline_preview
