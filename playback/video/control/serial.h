#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace playback_video_serial_control {

enum class DemuxSeekMode {
  Timeline,
  VideoAtOrBefore,
  VideoBeforeTarget,
};

struct TransitionPlan {
  bool valid = false;
  int serial = 0;
  int64_t displayTargetUs = 0;
  int64_t demuxTargetUs = 0;
  int64_t demuxWindowEndUs = 0;
  int64_t decoderPrerollTargetUs = 0;
  DemuxSeekMode demuxSeekMode = DemuxSeekMode::Timeline;
  bool signalCommandPending = false;
};

struct PendingSeek {
  bool valid = false;
  int serial = 0;
  int64_t demuxTargetUs = 0;
  int64_t demuxWindowEndUs = 0;
  int64_t displayTargetUs = 0;
  int64_t decoderPrerollTargetUs = 0;
  DemuxSeekMode demuxSeekMode = DemuxSeekMode::Timeline;
};

struct SeekRequest {
  uint64_t generation = 0;
  int64_t targetUs = 0;
};

struct PositionSnapshot {
  int currentSerial = 0;
  uint64_t latestSeekRequestGeneration = 0;
  uint64_t handledSeekRequestGeneration = 0;
  bool requestPending = false;
  bool seekPending = false;
  bool transitionPending = false;
  int pendingSeekSerial = 0;
  int seekInFlightSerial = 0;
  int64_t seekDisplayUs = 0;
  int64_t requestedSeekUs = 0;
  bool presentedPositionValid = false;
  int presentedPositionSerial = 0;
  int64_t presentedPositionUs = 0;
  int64_t positionUs = 0;
};

class Controller {
 public:
  void reset();
  void startSession(int initialSerial);

  SeekRequest publishSeekRequest(int64_t targetUs, int64_t maximumUs);
  SeekRequest publishRelativeSeekRequest(int64_t deltaUs,
                                         int64_t maximumUs);
  void acknowledgeSeekRequest(uint64_t generation);
  void notePresentedPosition(int serial, int64_t ptsUs);

  TransitionPlan beginTransition(int64_t targetUs, bool initDone,
                                 bool running);
  TransitionPlan beginTransition(int64_t displayTargetUs,
                                 int64_t demuxTargetUs, bool initDone,
                                 bool running);
  TransitionPlan beginTransition(int64_t displayTargetUs,
                                 int64_t demuxTargetUs,
                                 int64_t decoderPrerollTargetUs,
                                 bool initDone, bool running);
  TransitionPlan beginTransition(int64_t displayTargetUs,
                                 int64_t demuxTargetUs,
                                 int64_t demuxWindowEndUs,
                                 int64_t decoderPrerollTargetUs,
                                 bool initDone, bool running);
  TransitionPlan beginTransition(int64_t displayTargetUs,
                                 int64_t demuxTargetUs,
                                 int64_t demuxWindowEndUs,
                                 int64_t decoderPrerollTargetUs,
                                 DemuxSeekMode demuxSeekMode,
                                 bool initDone, bool running);
  PendingSeek claimPendingSeek();
  bool applySeekResult(int serial, int resultCode);
  void clearSeekFailure();
  bool clearPendingPresentation(int serial);

  int currentSerial() const;
  std::atomic<int>* currentSerialAtomic();
  bool seekPending() const;
  int pendingSeekSerial() const;
  int seekInFlightSerial() const;
  bool seekFailed() const;
  int64_t seekDisplayUs() const;
  PositionSnapshot positionSnapshot() const;
  int64_t presentationTargetUsForSerial(int serial) const;
  int64_t decoderPrerollTargetUsForSerial(int serial) const;

 private:
  mutable std::mutex transitionMutex_;
  std::atomic<int64_t> seekDisplayUs_{0};
  std::atomic<int> seekInFlightSerial_{0};
  std::atomic<bool> seekFailed_{false};
  std::atomic<bool> seekPending_{false};
  std::atomic<int64_t> seekTargetUs_{0};
  std::atomic<int64_t> demuxWindowEndUs_{0};
  std::atomic<int64_t> presentationTargetUs_{0};
  std::atomic<int64_t> decoderPrerollTargetUs_{0};
  std::atomic<int> demuxSeekMode_{static_cast<int>(DemuxSeekMode::Timeline)};
  std::atomic<int> currentSerial_{1};
  std::atomic<int> pendingSeekSerial_{0};
  std::atomic<int> presentationTargetSerial_{0};
  std::atomic<int> decoderPrerollTargetSerial_{0};
  uint64_t latestSeekRequestGeneration_ = 0;
  uint64_t handledSeekRequestGeneration_ = 0;
  int64_t requestedSeekUs_ = 0;
  bool presentedPositionValid_ = false;
  int presentedPositionSerial_ = 0;
  int64_t presentedPositionUs_ = 0;
};

}  // namespace playback_video_serial_control
