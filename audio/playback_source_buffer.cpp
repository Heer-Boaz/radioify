#include "playback_source_buffer.h"

#include <algorithm>
#include <cstring>

void PlaybackSourceBuffer::init(size_t capacityFrames, uint32_t channels) {
  capacityFrames_ = capacityFrames;
  channels_ = channels;
  data_.assign(capacityFrames_ * channels_, 0.0f);
  readPos_ = 0;
  writePos_ = 0;
  bufferedFrames_ = 0;
}

void PlaybackSourceBuffer::reset() {
  readPos_ = 0;
  writePos_ = 0;
  bufferedFrames_ = 0;
}

size_t PlaybackSourceBuffer::space() const {
  if (capacityFrames_ < bufferedFrames_) return 0;
  return capacityFrames_ - bufferedFrames_;
}

size_t PlaybackSourceBuffer::size() const {
  return bufferedFrames_;
}

size_t PlaybackSourceBuffer::read(float* out, size_t frames) {
  if (!out || capacityFrames_ == 0 || channels_ == 0 || frames == 0) {
    return 0;
  }
  size_t toRead = std::min(frames, bufferedFrames_);
  if (toRead == 0) return 0;

  size_t first = std::min(toRead, capacityFrames_ - readPos_);
  const size_t firstSamples = first * channels_;
  std::memcpy(out, data_.data() + readPos_ * channels_,
              firstSamples * sizeof(float));

  if (toRead > first) {
    const size_t second = toRead - first;
    std::memcpy(out + firstSamples, data_.data(),
                second * channels_ * sizeof(float));
  }

  readPos_ = (readPos_ + toRead) % capacityFrames_;
  bufferedFrames_ -= toRead;
  return toRead;
}

size_t PlaybackSourceBuffer::write(const float* in, size_t frames) {
  if (!in || capacityFrames_ == 0 || channels_ == 0 || frames == 0) {
    return 0;
  }
  size_t toWrite = std::min(frames, space());
  if (toWrite == 0) return 0;

  size_t first = std::min(toWrite, capacityFrames_ - writePos_);
  const size_t firstSamples = first * channels_;
  std::memcpy(data_.data() + writePos_ * channels_, in,
              firstSamples * sizeof(float));

  if (toWrite > first) {
    const size_t second = toWrite - first;
    std::memcpy(data_.data(), in + firstSamples,
                second * channels_ * sizeof(float));
  }

  writePos_ = (writePos_ + toWrite) % capacityFrames_;
  bufferedFrames_ += toWrite;
  return toWrite;
}
