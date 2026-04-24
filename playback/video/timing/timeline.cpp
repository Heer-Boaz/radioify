#include "timeline.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <string>

extern "C" {
#include <libavutil/rational.h>
}

#include "timing_log.h"

namespace playback_video_timeline {
namespace {

int64_t ptsToUs(int64_t pts, AVRational tb) {
  if (pts == AV_NOPTS_VALUE) {
    return AV_NOPTS_VALUE;
  }
  return av_rescale_q(pts, tb, AVRational{1, AV_TIME_BASE});
}

void logFmt(const std::filesystem::path& logPath, const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  int written = std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (written <= 0) {
    return;
  }
  if (written >= static_cast<int>(sizeof(buf))) {
    written = static_cast<int>(sizeof(buf)) - 1;
  }
  radioifyAppendTimingLogLine(logPath,
                              std::string(buf, static_cast<size_t>(written)));
}

}  // namespace

int64_t prerollSeekUs(int64_t targetUs, int64_t prerollUs) {
  return (std::max)(int64_t{0}, targetUs - (std::max)(int64_t{0}, prerollUs));
}

bool packetRelativeRangeUs(const AVPacket& packet, const AVStream* stream,
                           int64_t formatStartUs, int64_t* startUsOut,
                           int64_t* endUsOut) {
  if (!stream) {
    return false;
  }
  int64_t pts = packet.pts;
  if (pts == AV_NOPTS_VALUE) {
    pts = packet.dts;
  }
  if (pts == AV_NOPTS_VALUE) {
    return false;
  }
  int64_t startUs = ptsToUs(pts, stream->time_base);
  if (startUs == AV_NOPTS_VALUE) {
    return false;
  }
  startUs -= formatStartUs;
  if (startUs < 0) {
    startUs = 0;
  }

  int64_t durationUs = 0;
  if (packet.duration > 0) {
    durationUs = av_rescale_q(packet.duration, stream->time_base,
                              AVRational{1, AV_TIME_BASE});
    if (durationUs < 0) {
      durationUs = 0;
    }
  }

  if (startUsOut) {
    *startUsOut = startUs;
  }
  if (endUsOut) {
    *endUsOut = startUs + durationUs;
  }
  return true;
}

PrerollDiscard beginPrerollDiscard(int64_t targetUs) {
  PrerollDiscard discard;
  discard.targetUs = (std::max)(int64_t{0}, targetUs);
  discard.active = discard.targetUs > 0;
  return discard;
}

bool shouldDropPrerollPacket(PrerollDiscard* discard, const AVPacket& packet,
                             const AVStream* stream, int64_t formatStartUs) {
  if (!discard || !discard->active) {
    return false;
  }

  int64_t startUs = 0;
  int64_t endUs = 0;
  if (!packetRelativeRangeUs(packet, stream, formatStartUs, &startUs,
                             &endUs)) {
    discard->active = false;
    return false;
  }

  bool packetEndsBeforeTarget =
      endUs > startUs ? endUs <= discard->targetUs : startUs < discard->targetUs;
  if (packetEndsBeforeTarget) {
    return true;
  }

  discard->active = false;
  return false;
}

bool shouldDropPrerollFrame(int64_t targetUs, int64_t ptsUs,
                            int64_t durationUs, int64_t fallbackDurationUs) {
  if (targetUs <= 0 || ptsUs < 0) {
    return false;
  }
  int64_t effectiveDurationUs = durationUs;
  if (effectiveDurationUs <= 0) {
    effectiveDurationUs = fallbackDurationUs;
  }
  if (effectiveDurationUs <= 0) {
    effectiveDurationUs = 33333;
  }
  return ptsUs + effectiveDurationUs <= targetUs;
}

DemuxSeekResult seekPrimaryDemux(const DemuxSeekRequest& request) {
  DemuxSeekResult result;
  result.targetUs = (std::max)(int64_t{0}, request.targetUs);
  result.seekUs = (std::max)(int64_t{0}, request.seekUs);
  result.result = AVERROR(EINVAL);

  const char* tag = request.logTag ? request.logTag : "timeline_seek";
  if (!request.format) {
    logFmt(request.logPath, "%s target_us=%lld seek_us=%lld res=%d", tag,
           static_cast<long long>(result.targetUs),
           static_cast<long long>(result.seekUs), result.result);
    return result;
  }

  int64_t targetAbsUs = result.targetUs + request.formatStartUs;
  int64_t seekAbsUs = result.seekUs + request.formatStartUs;
  if (targetAbsUs < 0) {
    targetAbsUs = 0;
  }
  if (seekAbsUs < 0) {
    seekAbsUs = 0;
  }

  result.result = avformat_seek_file(request.format, -1, INT64_MIN, seekAbsUs,
                                     targetAbsUs, AVSEEK_FLAG_BACKWARD);
  if (result.result < 0 && request.videoStreamIndex >= 0 &&
      request.videoStreamIndex < static_cast<int>(request.format->nb_streams) &&
      request.videoTimeBase.num > 0 && request.videoTimeBase.den > 0) {
    int64_t streamTarget =
        av_rescale_q(seekAbsUs, AVRational{1, AV_TIME_BASE},
                     request.videoTimeBase);
    result.result = av_seek_frame(request.format, request.videoStreamIndex,
                                  streamTarget, AVSEEK_FLAG_BACKWARD);
  }

  result.seeked = result.result >= 0;
  if (result.seeked && request.flushOnSuccess) {
    avformat_flush(request.format);
  }
  logFmt(request.logPath,
         "%s target_us=%lld seek_us=%lld abs_us=%lld res=%d", tag,
         static_cast<long long>(result.targetUs),
         static_cast<long long>(result.seekUs),
         static_cast<long long>(seekAbsUs), result.result);
  return result;
}

}  // namespace playback_video_timeline
