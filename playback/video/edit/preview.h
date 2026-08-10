#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "playback/video/edit/timeline.h"

namespace playback_video_edit {

enum class PreviewDecisionKind : uint8_t {
  None,
  Seek,
  Complete,
};

struct PreviewDecision {
  PreviewDecisionKind kind = PreviewDecisionKind::None;
  int64_t targetUs = 0;
};

// Owns playback through the ordered clip sequence. The session executes seek
// decisions through the existing player transport, which keeps serial, audio
// reset, and clock reacquisition under their established owner.
class PreviewController {
 public:
  bool start(const std::vector<SourceRange>& ranges, int64_t* firstTargetUs);
  void stop();
  bool active() const { return active_; }

  PreviewDecision observePresentedFrame(int64_t positionUs,
                                        int64_t frameDurationUs,
                                        bool seekPending);

 private:
  bool active_ = false;
  size_t rangeIndex_ = 0;
  std::vector<SourceRange> ranges_;
};

}  // namespace playback_video_edit
