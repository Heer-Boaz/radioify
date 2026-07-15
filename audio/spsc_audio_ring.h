#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

class SpscAudioRing {
 public:
  void initialize(uint64_t capacityFrames, uint32_t channels);
  void discardBufferedFrames();

  uint64_t bufferedFrames() const;
  uint64_t writableFrames() const;
  uint64_t capacityFrames() const { return capacityFrames_; }
  uint64_t readPosition() const;
  uint64_t writePosition() const;

  uint64_t writeSome(const float* input, uint64_t frames);
  uint64_t readSome(float* output, uint64_t frames);

 private:
  std::vector<float> samples_;
  uint32_t channels_ = 0;
  uint64_t capacityFrames_ = 0;
  std::atomic<uint64_t> readPosition_{0};
  std::atomic<uint64_t> writePosition_{0};
};
