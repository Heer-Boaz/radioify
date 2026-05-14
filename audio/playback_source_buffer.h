#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class PlaybackSourceBuffer {
 public:
  void init(size_t capacityFrames, uint32_t channels);
  void reset();

  size_t space() const;
  size_t size() const;

  size_t read(float* out, size_t frames);
  size_t write(const float* in, size_t frames);

  uint32_t channels() const { return channels_; }
  size_t capacityFrames() const { return capacityFrames_; }

 private:
  std::vector<float> data_;
  size_t capacityFrames_ = 0;
  size_t readPos_ = 0;
  size_t writePos_ = 0;
  size_t bufferedFrames_ = 0;
  uint32_t channels_ = 0;
};
