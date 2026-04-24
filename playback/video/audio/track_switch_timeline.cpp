#include "track_switch_timeline.h"

#include <algorithm>

namespace playback_audio_track_switch_timeline {

void Controller::reset() {
  pending_.store(false, std::memory_order_relaxed);
  serial_.store(0, std::memory_order_relaxed);
  targetUs_.store(0, std::memory_order_relaxed);
  videoPreroll_ = {};
}

void Controller::request(int serial, int64_t targetUs) {
  if (serial <= 0) {
    reset();
    return;
  }

  targetUs_.store((std::max)(int64_t{0}, targetUs),
                  std::memory_order_relaxed);
  serial_.store(serial, std::memory_order_relaxed);
  pending_.store(true, std::memory_order_release);
}

PendingRequest Controller::claimPending() {
  PendingRequest request;
  if (!pending_.exchange(false, std::memory_order_acq_rel)) {
    return request;
  }
  request.valid = true;
  request.serial = serial_.load(std::memory_order_relaxed);
  request.targetUs = targetUs_.load(std::memory_order_relaxed);
  return request;
}

int64_t Controller::targetForSerial(int serial) const {
  if (serial <= 0 ||
      serial_.load(std::memory_order_relaxed) != serial) {
    return 0;
  }
  return targetUs_.load(std::memory_order_relaxed);
}

DemuxResult Controller::seekForPendingRequest(
    const PendingRequest& request, const DemuxContext& context) {
  DemuxResult result;
  if (!request.valid || request.serial <= 0) {
    return result;
  }

  result.handled = true;
  result.serial = request.serial;
  result.targetUs = (std::max)(int64_t{0}, request.targetUs);

  playback_video_timeline::DemuxSeekRequest seekRequest;
  seekRequest.format = context.format;
  seekRequest.videoStreamIndex = context.videoStreamIndex;
  seekRequest.videoTimeBase = context.videoTimeBase;
  seekRequest.formatStartUs = context.formatStartUs;
  seekRequest.targetUs = result.targetUs;
  seekRequest.seekUs = playback_video_timeline::prerollSeekUs(result.targetUs);
  seekRequest.logTag = "audio_track_switch_seek";
  seekRequest.logPath = context.logPath;
  result.seek = playback_video_timeline::seekPrimaryDemux(seekRequest);

  videoPreroll_ = result.seek.seeked
                      ? playback_video_timeline::beginPrerollDiscard(result.targetUs)
                      : playback_video_timeline::PrerollDiscard{};
  result.dropVideoBeforeUs = videoPreroll_.targetUs;
  return result;
}

bool Controller::shouldDropVideoPrerollPacket(const AVPacket& packet,
                                              const AVStream* stream,
                                              int64_t formatStartUs) {
  return playback_video_timeline::shouldDropPrerollPacket(
      &videoPreroll_, packet, stream, formatStartUs);
}

}  // namespace playback_audio_track_switch_timeline
