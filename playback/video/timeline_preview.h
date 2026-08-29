#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "core/native_wait_handle.h"
#include "playback/video/timeline_preview_cache.h"
#include "playback/video/timeline_preview_types.h"

namespace playback_video_timeline_preview {

// Asynchronous media provider for exact thumbnail requests.  Hover state and
// layout live outside this class; playback and audio state are never inputs.
class Provider {
 public:
  explicit Provider(CacheConfig cacheConfig = {});
  ~Provider();

  Provider(const Provider&) = delete;
  Provider& operator=(const Provider&) = delete;

  bool start(const Source& source);
  void requestStop();
  bool stopReady() const;
  bool finishStop();
  void stop();

  bool submit(const Request& request);
  // Advance the generation watermark and withdraw all older work.
  void cancelBefore(uint64_t requestId);

  std::optional<Result> takeResult();
  NativeWaitHandle changedWaitHandle() const;
  std::vector<NativeWaitHandle> stopWaitHandles() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_timeline_preview
