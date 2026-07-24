#include "seek_presentation.h"

void AudioSeekPresentation::reset() {
  requestedGeneration_.store(0, std::memory_order_relaxed);
  presentedGeneration_.store(0, std::memory_order_relaxed);
}

void AudioSeekPresentation::request() {
  requestedGeneration_.fetch_add(1, std::memory_order_release);
}

bool AudioSeekPresentation::pending() const {
  return presentedGeneration_.load(std::memory_order_acquire) !=
         requestedGeneration_.load(std::memory_order_acquire);
}

void AudioSeekPresentation::publishIfSettled(
    const std::atomic<bool>& backendRequestPending) {
  if (!pending() ||
      backendRequestPending.load(std::memory_order_acquire)) {
    return;
  }
  const uint64_t settledGeneration =
      requestedGeneration_.load(std::memory_order_acquire);
  if (backendRequestPending.load(std::memory_order_acquire)) {
    return;
  }
  presentedGeneration_.store(settledGeneration, std::memory_order_release);
}
