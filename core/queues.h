#pragma once

#include <atomic>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "videodecoder.h"

struct QueuedPacket {
  AVPacket pkt{};
  uint64_t serial = 0;
  bool flush = false;
  bool eof = false;
};

class PacketQueue {
 public:
  void init(size_t maxBytesIn);
  void abortQueue();
  void flush();
  void flush(uint64_t serial);
  bool pushPacket(const AVPacket* pkt, uint64_t serial, bool allowBlock,
                  const std::atomic<bool>* cancel, bool* queued);
  bool pushFlush(uint64_t serial);
  bool pushEof(uint64_t serial);
  bool pop(QueuedPacket* out);
  size_t size() const;

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<QueuedPacket> packets_;
  size_t bytes_ = 0;
  size_t maxBytes_ = 0;
  bool aborted_ = false;
};

struct QueuedFrame {
  size_t poolIndex = 0;
  int64_t ptsUs = 0;
  int64_t durationUs = 0;
  uint64_t serial = 0;
  VideoReadInfo info{};
  double decodeMs = 0.0;
};

class FrameQueue {
 public:
  void init(size_t maxFrames);
  void abort();
  void flush(uint64_t serial);
  bool acquireFrame(size_t* poolIndex, uint64_t serial, size_t maxQueue,
                    const std::atomic<bool>* running);
  bool push(const QueuedFrame& frame);
  bool peek(QueuedFrame* out) const;
  bool peekNext(QueuedFrame* out) const;
  bool pop(QueuedFrame* out);
  void release(size_t poolIndex);
  bool waitForFrame(std::chrono::milliseconds timeout,
                    const std::atomic<bool>* running,
                    const std::atomic<bool>* wake);
  size_t size() const;
  bool empty() const;
  uint64_t serial() const;
  VideoFrame& frame(size_t index);
  const VideoFrame& frame(size_t index) const;

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<QueuedFrame> queue_;
  std::deque<size_t> freeFrames_;
  std::vector<VideoFrame> pool_;
  size_t maxFrames_ = 0;
  uint64_t serial_ = 0;
  bool aborted_ = false;
};
