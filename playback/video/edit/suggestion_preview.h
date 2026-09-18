#pragma once

#include <cstdint>
#include <optional>

namespace playback_video_edit {

// A transport audition, not an edit or a replacement playback sequence.
// The workspace requests seeks; the session owns synchronized A/V play/pause.
class SuggestionPreview {
 public:
  enum class Update { None, EndReached, Superseded };
  bool start(int64_t timelineStartUs, int64_t timelineEndUs, uint64_t seekGeneration) {
    if (timelineStartUs < 0 || timelineEndUs <= timelineStartUs)
      return false;
    endUs_ = timelineEndUs;
    generation_ = seekGeneration;
    return true;
  }
  bool active() const { return endUs_.has_value(); }
  bool stop() {
    const bool wasActive = active();
    endUs_.reset();
    return wasActive;
  }
  Update observe(int64_t positionUs, uint64_t latestSeek, uint64_t handledSeek,
                 bool seekPending, bool playbackEnded) {
    if (!endUs_) return Update::None;
    if (latestSeek != generation_) {
      stop();
      return Update::Superseded;
    }
    if (seekPending || handledSeek < generation_) return Update::None;
    if (playbackEnded || positionUs >= *endUs_) {
      stop();
      return Update::EndReached;
    }
    return Update::None;
  }
 private:
  std::optional<int64_t> endUs_;
  uint64_t generation_ = 0;
};

}  // namespace playback_video_edit
