#pragma once

#include <cstdint>
#include <memory>

#include "core/native_wait_handle.h"
#include "playback/video/chapter/backend.h"

namespace playback_video_chapters {

// Process-wide owner for the one active video's automatic chapter analysis.
// Sessions own request identities and immutable presentation snapshots; the
// worker, model installation and native inference lifecycle never belong to a
// renderer.
class Service {
 public:
  using RequestId = std::uint64_t;

  explicit Service(std::unique_ptr<Backend> backend = createDefaultBackend());
  ~Service();

  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;

  RequestId start(AnalysisRequest request);
  void cancel(RequestId requestId);

  Snapshot snapshot(RequestId requestId) const;
  bool requestInstallation(RequestId requestId);
  bool cancelInstallation(RequestId requestId);

  // Playback is the foreground GPU owner. False interrupts sparse decode and
  // aborts native VLM inference. Native GPU allocations are released while
  // validated, request-bound inference stages remain resumable in private
  // backend state.
  void setBackgroundGpuAllowed(RequestId requestId, bool allowed);

  NativeWaitHandle changedWaitHandle() const;
  bool consumeChanged();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_chapters
