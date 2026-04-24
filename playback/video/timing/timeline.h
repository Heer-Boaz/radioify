#pragma once

#include <cstdint>
#include <filesystem>

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
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
