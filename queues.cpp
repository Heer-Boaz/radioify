#include "queues.h"

#include <algorithm>
#include <chrono>

void PacketQueue::init(size_t maxBytesIn) {
  std::lock_guard<std::mutex> lock(mutex_);
  maxBytes_ = maxBytesIn;
  aborted_ = false;
  bytes_ = 0;
  packets_.clear();
}

void PacketQueue::abortQueue() {
  std::lock_guard<std::mutex> lock(mutex_);
  aborted_ = true;
  cv_.notify_all();
}

void PacketQueue::flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& item : packets_) {
    av_packet_unref(&item.pkt);
  }
  packets_.clear();
  bytes_ = 0;
  cv_.notify_all();
}

void PacketQueue::flush(uint64_t serial) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& item : packets_) {
    av_packet_unref(&item.pkt);
  }
  packets_.clear();
  bytes_ = 0;
  QueuedPacket item;
  av_init_packet(&item.pkt);
  item.serial = serial;
  item.flush = true;
  item.eof = false;
  packets_.push_back(std::move(item));
  cv_.notify_all();
}

bool PacketQueue::pushPacket(const AVPacket* pkt, uint64_t serial,
                             bool allowBlock, const std::atomic<bool>* cancel,
                             bool* queued) {
  if (queued) {
    *queued = false;
  }
  if (!pkt) return false;
  size_t packetBytes = static_cast<size_t>(std::max(0, pkt->size));
  std::unique_lock<std::mutex> lock(mutex_);
  if (allowBlock) {
    while (!aborted_ && !packets_.empty() &&
           bytes_ + packetBytes > maxBytes_) {
      if (cancel && cancel->load(std::memory_order_relaxed)) {
        return true;
      }
      cv_.wait_for(lock, std::chrono::milliseconds(2));
    }
  } else {
    if (aborted_) return false;
    if (!packets_.empty() && bytes_ + packetBytes > maxBytes_) {
      return true;
    }
  }
  if (aborted_) return false;
  if (cancel && cancel->load(std::memory_order_relaxed)) {
    return true;
  }
  QueuedPacket item;
  av_init_packet(&item.pkt);
  if (av_packet_ref(&item.pkt, pkt) < 0) {
    return false;
  }
  item.serial = serial;
  item.flush = false;
  item.eof = false;
  packets_.push_back(std::move(item));
  bytes_ += packetBytes;
  if (queued) {
    *queued = true;
  }
  cv_.notify_all();
  return true;
}

bool PacketQueue::pushFlush(uint64_t serial) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (aborted_) return false;
  QueuedPacket item;
  av_init_packet(&item.pkt);
  item.serial = serial;
  item.flush = true;
  item.eof = false;
  packets_.push_back(std::move(item));
  cv_.notify_all();
  return true;
}

bool PacketQueue::pushEof(uint64_t serial) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (aborted_) return false;
  QueuedPacket item;
  av_init_packet(&item.pkt);
  item.serial = serial;
  item.flush = false;
  item.eof = true;
  packets_.push_back(std::move(item));
  cv_.notify_all();
  return true;
}

bool PacketQueue::pop(QueuedPacket* out) {
  if (!out) return false;
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [&]() { return aborted_ || !packets_.empty(); });
  if (aborted_) return false;
  *out = std::move(packets_.front());
  packets_.pop_front();
  if (!out->flush && !out->eof) {
    bytes_ -= static_cast<size_t>(std::max(0, out->pkt.size));
  }
  cv_.notify_all();
  return true;
}

size_t PacketQueue::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return packets_.size();
}

void FrameQueue::init(size_t maxFrames) {
  std::lock_guard<std::mutex> lock(mutex_);
  maxFrames_ = maxFrames;
  aborted_ = false;
  serial_ = 0;
  queue_.clear();
  pool_.assign(maxFrames_ + 1, VideoFrame{});
  freeFrames_.clear();
  for (size_t i = 0; i < pool_.size(); ++i) {
    freeFrames_.push_back(i);
  }
  cv_.notify_all();
}

void FrameQueue::abort() {
  std::lock_guard<std::mutex> lock(mutex_);
  aborted_ = true;
  cv_.notify_all();
}

void FrameQueue::flush(uint64_t serial) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& item : queue_) {
    if (item.poolIndex < pool_.size()) {
      pool_[item.poolIndex].hwFrameRef.reset();
      pool_[item.poolIndex].hwTexture.Reset();
      pool_[item.poolIndex].hwTextureArrayIndex = 0;
    }
    freeFrames_.push_back(item.poolIndex);
  }
  queue_.clear();
  serial_ = serial;
  cv_.notify_all();
}

bool FrameQueue::acquireFrame(size_t* poolIndex, uint64_t serial,
                              size_t maxQueue,
                              const std::atomic<bool>* running) {
  if (!poolIndex) return false;
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [&]() {
    bool run = running ? running->load() : true;
    if (aborted_ || !run) return true;
    if (serial != serial_) return true;
    return !freeFrames_.empty() && queue_.size() < maxQueue;
  });
  bool run = running ? running->load() : true;
  if (aborted_ || !run) return false;
  if (serial != serial_) return false;
  if (freeFrames_.empty()) return false;
  *poolIndex = freeFrames_.front();
  freeFrames_.pop_front();
  return true;
}

bool FrameQueue::push(const QueuedFrame& frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (aborted_) {
    return false;
  }
  if (queue_.size() >= maxFrames_) {
    if (frame.poolIndex < pool_.size()) {
      pool_[frame.poolIndex].hwFrameRef.reset();
      pool_[frame.poolIndex].hwTexture.Reset();
      pool_[frame.poolIndex].hwTextureArrayIndex = 0;
    }
    freeFrames_.push_back(frame.poolIndex);
    return false;
  }
  queue_.push_back(frame);
  cv_.notify_all();
  return true;
}

bool FrameQueue::peek(QueuedFrame* out) const {
  if (!out) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.empty()) return false;
  *out = queue_.front();
  return true;
}

bool FrameQueue::pop(QueuedFrame* out) {
  if (!out) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.empty()) return false;
  *out = queue_.front();
  queue_.pop_front();
  cv_.notify_all();
  return true;
}

void FrameQueue::release(size_t poolIndex) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (poolIndex < pool_.size()) {
    pool_[poolIndex].hwFrameRef.reset();
    pool_[poolIndex].hwTexture.Reset();
    pool_[poolIndex].hwTextureArrayIndex = 0;
  }
  freeFrames_.push_back(poolIndex);
  cv_.notify_all();
}

bool FrameQueue::waitForFrame(std::chrono::milliseconds timeout,
                              const std::atomic<bool>* running,
                              const std::atomic<bool>* wake) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait_for(lock, timeout, [&]() {
    bool run = running ? running->load() : true;
    bool wakeNow = wake ? wake->load() : false;
    return aborted_ || !run || wakeNow || !queue_.empty();
  });
  return !queue_.empty();
}

size_t FrameQueue::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

bool FrameQueue::empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.empty();
}

uint64_t FrameQueue::serial() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return serial_;
}

VideoFrame& FrameQueue::frame(size_t index) {
  return pool_[index];
}

const VideoFrame& FrameQueue::frame(size_t index) const {
  return pool_[index];
}
