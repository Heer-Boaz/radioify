#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/native_wait_handle.h"
#include "tui/style.h"

struct BrowserEntry;

struct BrowserThumbnailCell {
  wchar_t ch = L' ';
  Color fg{255, 255, 255};
  Color bg{0, 0, 0};
  bool hasBg = false;
};

struct BrowserThumbnail {
  int width = 0;
  int height = 0;
  std::vector<BrowserThumbnailCell> cells;
};

// Owner-scoped asynchronous cache for browser artwork previews. The cache
// owns and joins its worker; callers only submit typed media requests and wait
// on one completion signal.
class BrowserThumbnailCache {
 public:
  enum class MediaKind : std::uint8_t {
    Image,
    Video,
    Audio,
  };

  struct Lookup {
    std::shared_ptr<const BrowserThumbnail> thumbnail;
    bool pending = false;
    bool enqueued = false;
  };

  BrowserThumbnailCache();
  ~BrowserThumbnailCache();

  BrowserThumbnailCache(const BrowserThumbnailCache&) = delete;
  BrowserThumbnailCache& operator=(const BrowserThumbnailCache&) = delete;

  void configureTargetSize(int width, int height);
  Lookup lookupOrRequest(const BrowserEntry& entry, MediaKind kind, int width,
                         int height, bool allowEnqueue);

  NativeWaitHandle waitHandle() const;
  bool consumeReady();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
