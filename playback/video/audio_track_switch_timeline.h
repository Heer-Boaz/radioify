#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>

#include "playback_timeline.h"

namespace playback_audio_track_switch_timeline {

struct PendingRequest {
  bool valid = false;
  int serial = 0;
  int64_t targetUs = 0;
};

struct DemuxContext {
  AVFormatContext* format = nullptr;
  int videoStreamIndex = -1;
  AVRational videoTimeBase{0, 1};
  int64_t formatStartUs = 0;
  std::filesystem::path logPath;
};

struct DemuxResult {
  bool handled = false;
  int serial = 0;
  int64_t targetUs = 0;
  playback_timeline::DemuxSeekResult seek;
  int64_t dropVideoBeforeUs = 0;
};

class Controller {
 public:
  void reset();
  void request(int serial, int64_t targetUs);
  PendingRequest claimPending();
  int64_t targetForSerial(int serial) const;
  DemuxResult seekForPendingRequest(const PendingRequest& request,
                                    const DemuxContext& context);
  bool shouldDropVideoPrerollPacket(const AVPacket& packet,
                                    const AVStream* stream,
                                    int64_t formatStartUs);

 private:
  std::atomic<bool> pending_{false};
  std::atomic<int> serial_{0};
  std::atomic<int64_t> targetUs_{0};
  playback_timeline::PrerollDiscard videoPreroll_;
};

}  // namespace playback_audio_track_switch_timeline
