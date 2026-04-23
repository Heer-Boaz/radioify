#pragma once

#include <atomic>
#include <cstdint>

namespace playback_serial_control {

struct TransitionPlan {
  bool valid = false;
  int serial = 0;
  int64_t displayTargetUs = 0;
  int64_t demuxTargetUs = 0;
  bool signalCommandPending = false;
};

struct PendingSeek {
  bool valid = false;
  int serial = 0;
  int64_t demuxTargetUs = 0;
  int64_t displayTargetUs = 0;
};

class Controller {
 public:
  void reset();
  void startSession(int initialSerial);

  TransitionPlan beginTransition(int64_t targetUs, bool initDone,
                                 bool running);
  PendingSeek claimPendingSeek();
  bool applySeekResult(int serial, int resultCode);
  void clearSeekFailure();
  void clearPendingPresentation(int serial);

  int currentSerial() const;
  std::atomic<int>* currentSerialAtomic();
  bool seekPending() const;
  int pendingSeekSerial() const;
  int seekInFlightSerial() const;
  bool seekFailed() const;
  int64_t seekDisplayUs() const;
  int64_t presentationTargetUsForSerial(int serial) const;

 private:
  std::atomic<int64_t> seekDisplayUs_{0};
  std::atomic<int> seekInFlightSerial_{0};
  std::atomic<bool> seekFailed_{false};
  std::atomic<bool> seekPending_{false};
  std::atomic<int64_t> seekTargetUs_{0};
  std::atomic<int64_t> presentationTargetUs_{0};
  std::atomic<int> currentSerial_{1};
  std::atomic<int> pendingSeekSerial_{0};
  std::atomic<int> presentationTargetSerial_{0};
};

}  // namespace playback_serial_control
