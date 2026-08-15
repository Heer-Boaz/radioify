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

enum class PreviewEventType : uint8_t {
  FrameChanged,
  RenderFailed,
};

struct PreviewEvent {
  PreviewEventType type = PreviewEventType::FrameChanged;
  std::string message;
};

// A one-transition, in-memory render cache. The worker evaluates the same
// motion filter definition as export, while Player's transport keeps decoding
// the immutable source timeline. Until a render is ready, callers simply keep
// the ordinary hard-cut frame.
class PreviewCache {
 public:
  using EventCallback = std::function<void(PreviewEvent)>;

  PreviewCache();
  ~PreviewCache();

  PreviewCache(const PreviewCache&) = delete;
  PreviewCache& operator=(const PreviewCache&) = delete;

  bool start(const PreviewSource& source, EventCallback eventCallback);
  void stop();

  void setPlan(uint64_t compositionId, std::shared_ptr<const RenderPlan> plan,
               int64_t focusPresentationUs);
  void prefetchAround(uint64_t compositionId, int64_t presentationUs);

  bool copyFrame(uint64_t compositionId, int64_t presentationUs,
                 int64_t presentationDurationUs, VideoFrame* out) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_composition
