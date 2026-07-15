#include "spsc_audio_ring.h"

#include <algorithm>
#include <cassert>
#include <cstring>

void SpscAudioRing::initialize(uint64_t capacityFrames, uint32_t channels) {
  assert(capacityFrames > 0 && channels > 0);
  capacityFrames_ = capacityFrames;
  channels_ = channels;
  samples_.assign(capacityFrames_ * channels_, 0.0f);
  readPosition_.store(0, std::memory_order_relaxed);
  writePosition_.store(0, std::memory_order_relaxed);
}

void SpscAudioRing::discardBufferedFrames() {
  readPosition_.store(writePosition_.load(std::memory_order_acquire),
                      std::memory_order_release);
}

uint64_t SpscAudioRing::bufferedFrames() const {
  const uint64_t read = readPosition_.load(std::memory_order_acquire);
  const uint64_t written = writePosition_.load(std::memory_order_acquire);
  assert(written >= read && written - read <= capacityFrames_);
  return written - read;
}

uint64_t SpscAudioRing::writableFrames() const {
  return capacityFrames_ - bufferedFrames();
}

uint64_t SpscAudioRing::readPosition() const {
  return readPosition_.load(std::memory_order_acquire);
}

uint64_t SpscAudioRing::writePosition() const {
  return writePosition_.load(std::memory_order_acquire);
}

uint64_t SpscAudioRing::writeSome(const float* input, uint64_t frames) {
  assert(input && capacityFrames_ > 0 && channels_ > 0);
  const uint64_t written = writePosition_.load(std::memory_order_relaxed);
  const uint64_t read = readPosition_.load(std::memory_order_acquire);
  assert(written >= read && written - read <= capacityFrames_);
  const uint64_t available = capacityFrames_ - (written - read);
  const uint64_t count = std::min(frames, available);
  if (count == 0) return 0;

  const uint64_t writeIndex = written % capacityFrames_;
  const uint64_t first = std::min(count, capacityFrames_ - writeIndex);
  std::memcpy(samples_.data() + writeIndex * channels_, input,
              first * channels_ * sizeof(float));
  if (count > first) {
    std::memcpy(samples_.data(), input + first * channels_,
                (count - first) * channels_ * sizeof(float));
  }
  writePosition_.store(written + count, std::memory_order_release);
  return count;
}

uint64_t SpscAudioRing::readSome(float* output, uint64_t frames) {
  assert(output && capacityFrames_ > 0 && channels_ > 0);
  const uint64_t read = readPosition_.load(std::memory_order_relaxed);
  const uint64_t written = writePosition_.load(std::memory_order_acquire);
  assert(written >= read && written - read <= capacityFrames_);
  const uint64_t count = std::min(frames, written - read);
  if (count == 0) return 0;

  const uint64_t readIndex = read % capacityFrames_;
  const uint64_t first = std::min(count, capacityFrames_ - readIndex);
  std::memcpy(output, samples_.data() + readIndex * channels_,
              first * channels_ * sizeof(float));
  if (count > first) {
    std::memcpy(output + first * channels_, samples_.data(),
                (count - first) * channels_ * sizeof(float));
  }
  readPosition_.store(read + count, std::memory_order_release);
  return count;
}
