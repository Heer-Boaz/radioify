#pragma once

#include <cstdint>
#include <filesystem>

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

namespace playback_video_timeline {

inline constexpr int64_t kDefaultPrerollUs = 1000000;

struct DemuxSeekRequest {
  AVFormatContext* format = nullptr;
  int videoStreamIndex = -1;
  AVRational videoTimeBase{0, 1};
  int64_t formatStartUs = 0;
  int64_t targetUs = 0;
  int64_t seekUs = 0;
  bool preferVideoStream = false;
  bool flushOnSuccess = true;
  const char* logTag = "timeline_seek";
  std::filesystem::path logPath;
};

struct DemuxSeekResult {
  bool seeked = false;
  int result = 0;
  int64_t targetUs = 0;
  int64_t seekUs = 0;
};

struct PrerollDiscard {
  bool active = false;
  int64_t targetUs = 0;
};

inline int64_t videoStreamTimestampFloorForUs(int64_t absoluteUs,
                                              AVRational videoTimeBase) {
  if (absoluteUs < 0) {
    absoluteUs = 0;
  }
  return av_rescale_q_rnd(
      absoluteUs, AVRational{1, AV_TIME_BASE}, videoTimeBase,
      static_cast<AVRounding>(AV_ROUND_DOWN | AV_ROUND_PASS_MINMAX));
}

int64_t prerollSeekUs(int64_t targetUs,
                      int64_t prerollUs = kDefaultPrerollUs);

bool packetRelativeRangeUs(const AVPacket& packet, const AVStream* stream,
                           int64_t formatStartUs, int64_t* startUsOut,
                           int64_t* endUsOut);

PrerollDiscard beginPrerollDiscard(int64_t targetUs);

bool shouldDropPrerollPacket(PrerollDiscard* discard, const AVPacket& packet,
                             const AVStream* stream, int64_t formatStartUs);

bool shouldDropPrerollFrame(int64_t targetUs, int64_t ptsUs,
                            int64_t durationUs, int64_t fallbackDurationUs);

DemuxSeekResult seekPrimaryDemux(const DemuxSeekRequest& request);

}  // namespace playback_video_timeline
