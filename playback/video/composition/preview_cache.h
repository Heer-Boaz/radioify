#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "playback/video/composition/render_plan.h"
#include "playback/video/decoder.h"

namespace playback_video_composition {

struct PreviewSource {
  std::filesystem::path path;
  int videoStreamIndex = -1;
};

// A one-transition, in-memory render cache. The worker evaluates the same
// motion filter definition as export, while Player's transport keeps decoding
// the immutable source timeline. Until a render is ready, callers simply keep
// the ordinary hard-cut frame.
class PreviewCache {
 public:
  // Empty means the cached visual changed; a message reports a failed
  // background render without replacing the currently displayed base frame.
  using StatusCallback = std::function<void(const std::string&)>;

  PreviewCache();
  ~PreviewCache();

  PreviewCache(const PreviewCache&) = delete;
  PreviewCache& operator=(const PreviewCache&) = delete;

  bool start(const PreviewSource& source, StatusCallback statusCallback);
  void stop();

  void setPlan(uint64_t revision, std::shared_ptr<const RenderPlan> plan,
               int64_t focusPresentationUs);
  void requestNear(int64_t presentationUs);

  bool copyFrame(int64_t presentationUs, int64_t presentationDurationUs,
                 VideoFrame* out) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_composition
