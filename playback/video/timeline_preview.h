#pragma once

#include <filesystem>
#include <memory>

#include "core/native_wait_handle.h"
#include "playback/video/timeline_preview_types.h"

namespace playback_video_timeline_preview {

class Service {
 public:
  Service();
  ~Service();

  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;

  bool start(const std::filesystem::path& path, int videoStreamIndex,
             int64_t durationUs, int sourceWidth, int sourceHeight);
  void stop();

  // progressUnits is the number of independently addressable positions in the
  // active progress bar (terminal cells or window text-grid cells).
  bool request(double ratio, int progressUnits);
  bool hide();

  Snapshot snapshot() const;
  bool consumeChanged();
  NativeWaitHandle changedWaitHandle() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_timeline_preview
