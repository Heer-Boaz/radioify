#include "spsc_audio_timeline.h"

#include <cassert>

void SpscAudioTimeline::initialize(uint64_t capacityAnchors) {
  assert(capacityAnchors > 0);
  capacityAnchors_ = capacityAnchors;
  anchors_ = std::make_unique<Slot[]>(capacityAnchors_);
  readPosition_.store(0, std::memory_order_relaxed);
  writePosition_.store(0, std::memory_order_relaxed);
}

void SpscAudioTimeline::discardAnchors() {
  readPosition_.store(writePosition_.load(std::memory_order_acquire),
                      std::memory_order_release);
}

uint64_t SpscAudioTimeline::bufferedAnchors() const {
  const uint64_t read = readPosition_.load(std::memory_order_acquire);
  const uint64_t written = writePosition_.load(std::memory_order_acquire);
  assert(written >= read && written - read <= capacityAnchors_);
  return written - read;
}

bool SpscAudioTimeline::append(const AudioTimelineAnchor& anchor) {
  assert(anchors_ && capacityAnchors_ > 0);
  const uint64_t written = writePosition_.load(std::memory_order_relaxed);
  const uint64_t read = readPosition_.load(std::memory_order_acquire);
  assert(written >= read && written - read <= capacityAnchors_);
  if (written - read == capacityAnchors_) return false;

  Slot& slot = anchors_[written % capacityAnchors_];
  slot.sequence.store(0, std::memory_order_seq_cst);
  slot.framePosition.store(anchor.framePosition, std::memory_order_seq_cst);
  slot.ptsUs.store(anchor.ptsUs, std::memory_order_seq_cst);
  slot.serial.store(anchor.serial, std::memory_order_seq_cst);
  slot.sequence.store(written + 1, std::memory_order_seq_cst);
  writePosition_.store(written + 1, std::memory_order_release);
  return true;
}

bool SpscAudioTimeline::peek(uint64_t offset,
                             AudioTimelineAnchor* anchor) const {
  assert(anchor && anchors_ && capacityAnchors_ > 0);
  constexpr int kSnapshotAttempts = 4;
  for (int attempt = 0; attempt < kSnapshotAttempts; ++attempt) {
    const uint64_t read = readPosition_.load(std::memory_order_acquire);
    const uint64_t written = writePosition_.load(std::memory_order_acquire);
    if (written < read || offset >= written - read) return false;

    const uint64_t position = read + offset;
    AudioTimelineAnchor snapshot;
    if (readSlot(position, &snapshot) &&
        readPosition_.load(std::memory_order_acquire) == read) {
      *anchor = snapshot;
      return true;
    }
  }
  return false;
}

bool SpscAudioTimeline::findAnchorForFramePosition(
    uint64_t framePosition,
    AudioTimelineAnchor* anchor) const {
  assert(anchor && anchors_ && capacityAnchors_ > 0);
  constexpr int kSnapshotAttempts = 4;
  for (int attempt = 0; attempt < kSnapshotAttempts; ++attempt) {
    const uint64_t read = readPosition_.load(std::memory_order_acquire);
    const uint64_t written = writePosition_.load(std::memory_order_acquire);
    if (written <= read) return false;

    AudioTimelineAnchor candidate;
    if (!readSlot(read, &candidate)) continue;
    bool snapshotValid = true;
    for (uint64_t position = read + 1; position < written; ++position) {
      AudioTimelineAnchor next;
      if (!readSlot(position, &next)) {
        snapshotValid = false;
        break;
      }
      if (next.framePosition > framePosition) break;
      candidate = next;
    }
    if (snapshotValid &&
        readPosition_.load(std::memory_order_acquire) == read) {
      *anchor = candidate;
      return true;
    }
  }
  return false;
}

bool SpscAudioTimeline::popFront() {
  assert(anchors_ && capacityAnchors_ > 0);
  const uint64_t read = readPosition_.load(std::memory_order_relaxed);
  const uint64_t written = writePosition_.load(std::memory_order_acquire);
  if (read == written) return false;
  assert(written > read && written - read <= capacityAnchors_);
  readPosition_.store(read + 1, std::memory_order_release);
  return true;
}

bool SpscAudioTimeline::readSlot(uint64_t position,
                                 AudioTimelineAnchor* anchor) const {
  const Slot& slot = anchors_[position % capacityAnchors_];
  const uint64_t expectedSequence = position + 1;
  if (slot.sequence.load(std::memory_order_seq_cst) != expectedSequence) {
    return false;
  }
  anchor->framePosition =
      slot.framePosition.load(std::memory_order_seq_cst);
  anchor->ptsUs = slot.ptsUs.load(std::memory_order_seq_cst);
  anchor->serial = slot.serial.load(std::memory_order_seq_cst);
  return slot.sequence.load(std::memory_order_seq_cst) == expectedSequence;
}

void SpscAudioEventTimeline::initialize(uint64_t capacityEvents) {
  assert(capacityEvents > 0);
  capacityEvents_ = capacityEvents;
  events_ = std::make_unique<Slot[]>(capacityEvents_);
  readPosition_.store(0, std::memory_order_relaxed);
  writePosition_.store(0, std::memory_order_relaxed);
}

void SpscAudioEventTimeline::discardEvents() {
  readPosition_.store(writePosition_.load(std::memory_order_acquire),
                      std::memory_order_release);
}

uint64_t SpscAudioEventTimeline::bufferedEvents() const {
  const uint64_t read = readPosition_.load(std::memory_order_acquire);
  const uint64_t written = writePosition_.load(std::memory_order_acquire);
  assert(written >= read && written - read <= capacityEvents_);
  return written - read;
}

bool SpscAudioEventTimeline::append(
    const AudioPlaybackEventAnchor& anchor) {
  assert(events_ && capacityEvents_ > 0);
  const uint64_t written = writePosition_.load(std::memory_order_relaxed);
  const uint64_t read = readPosition_.load(std::memory_order_acquire);
  assert(written >= read && written - read <= capacityEvents_);
  if (written - read == capacityEvents_) return false;

  Slot& slot = events_[written % capacityEvents_];
  slot.sequence.store(0, std::memory_order_seq_cst);
  slot.framePosition.store(anchor.framePosition, std::memory_order_seq_cst);
  slot.event.store(static_cast<uint8_t>(anchor.event),
                   std::memory_order_seq_cst);
  slot.sequence.store(written + 1, std::memory_order_seq_cst);
  writePosition_.store(written + 1, std::memory_order_release);
  return true;
}

bool SpscAudioEventTimeline::popBefore(
    uint64_t framePosition,
    AudioPlaybackEventAnchor* anchor) {
  assert(anchor && events_ && capacityEvents_ > 0);
  constexpr int kSnapshotAttempts = 4;
  for (int attempt = 0; attempt < kSnapshotAttempts; ++attempt) {
    uint64_t read = readPosition_.load(std::memory_order_acquire);
    const uint64_t written = writePosition_.load(std::memory_order_acquire);
    if (written <= read) return false;

    AudioPlaybackEventAnchor snapshot;
    if (!readSlot(read, &snapshot)) continue;
    if (snapshot.framePosition >= framePosition) return false;

    // A reset may advance the read position from the producer thread. CAS
    // prevents this consumer from overwriting that discard with a stale read.
    if (readPosition_.compare_exchange_strong(
            read, read + 1, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      *anchor = snapshot;
      return true;
    }
  }
  return false;
}

bool SpscAudioEventTimeline::readSlot(
    uint64_t position,
    AudioPlaybackEventAnchor* anchor) const {
  const Slot& slot = events_[position % capacityEvents_];
  const uint64_t expectedSequence = position + 1;
  if (slot.sequence.load(std::memory_order_seq_cst) != expectedSequence) {
    return false;
  }
  anchor->framePosition =
      slot.framePosition.load(std::memory_order_seq_cst);
  anchor->event = static_cast<AudioPlaybackEvent>(
      slot.event.load(std::memory_order_seq_cst));
  return slot.sequence.load(std::memory_order_seq_cst) == expectedSequence;
}
