#pragma once

#include <atomic>
#include <cstdint>

class AudioSeekPresentation {
 public:
  void reset();
  void request();
  bool pending() const;
  void publishIfSettled(const std::atomic<bool>& backendRequestPending);

 private:
  std::atomic<uint64_t> requestedGeneration_{0};
  std::atomic<uint64_t> presentedGeneration_{0};
};
