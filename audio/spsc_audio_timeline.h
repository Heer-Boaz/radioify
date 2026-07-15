#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

struct AudioTimelineAnchor {
  uint64_t framePosition = 0;
  int64_t ptsUs = 0;
  int serial = 0;
};

class SpscAudioTimeline {
 public:
  void initialize(uint64_t capacityAnchors);
  void discardAnchors();

  uint64_t bufferedAnchors() const;
  bool append(const AudioTimelineAnchor& anchor);
  bool peek(uint64_t offset, AudioTimelineAnchor* anchor) const;
  bool findAnchorForFramePosition(uint64_t framePosition,
                                  AudioTimelineAnchor* anchor) const;
  bool popFront();

 private:
  struct Slot {
    std::atomic<uint64_t> sequence{0};
    std::atomic<uint64_t> framePosition{0};
    std::atomic<int64_t> ptsUs{0};
    std::atomic<int> serial{0};
  };

  bool readSlot(uint64_t position, AudioTimelineAnchor* anchor) const;

  std::unique_ptr<Slot[]> anchors_;
  uint64_t capacityAnchors_ = 0;
  std::atomic<uint64_t> readPosition_{0};
  std::atomic<uint64_t> writePosition_{0};
};
