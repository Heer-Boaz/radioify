#include "videodecoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mfapi.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>

namespace {
void setError(std::string* error, const char* message) {
  if (error) *error = message;
}

std::string ffmpegError(int err) {
  char buf[256];
  buf[0] = '\0';
  av_strerror(err, buf, sizeof(buf));
  return std::string(buf);
}

uint32_t mapColorMatrix(AVColorSpace space) {
  switch (space) {
    case AVCOL_SPC_BT709:
      return MFVideoTransferMatrix_BT709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
      return MFVideoTransferMatrix_BT601;
    default:
      return MFVideoTransferMatrix_BT709;
  }
}

bool mapFullRange(AVColorRange range) {
  return range == AVCOL_RANGE_JPEG;
}
}  // namespace

struct VideoDecoder::Impl {
  AVFormatContext* fmt = nullptr;
  AVCodecContext* codec = nullptr;
  AVFrame* frame = nullptr;
  AVFrame* scratch = nullptr;
  AVPacket* packet = nullptr;
  SwsContext* sws = nullptr;
  int streamIndex = -1;
  AVRational timeBase{0, 1};
  int width = 0;
  int height = 0;
  int targetW = 0;
  int targetH = 0;
  bool atEnd = false;
  bool eof = false;
  int64_t duration100ns = 0;
  int64_t formatStartUs = 0;
  int64_t streamStartUs = 0;
  uint32_t yuvMatrix = MFVideoTransferMatrix_BT709;
  bool fullRange = true;
};

VideoDecoder::~VideoDecoder() { uninit(); }

bool VideoDecoder::init(const std::filesystem::path& path, std::string* error,
                        bool, bool) {
  uninit();

  AVFormatContext* fmt = nullptr;
  AVDictionary* options = nullptr;
  av_dict_set_int(&options, "probesize", 64 * 1024, 0);
  av_dict_set_int(&options, "analyzeduration", 0, 0);

  int openErr = avformat_open_input(&fmt, path.string().c_str(), nullptr,
                                    &options);
  av_dict_free(&options);
  if (openErr < 0) {
    std::string msg = "Failed to open video: " + ffmpegError(openErr);
    setError(error, msg.c_str());
    return false;
  }

  fmt->flags |= AVFMT_FLAG_NOBUFFER;
  fmt->max_analyze_duration = 0;
  fmt->probesize = 64 * 1024;

  int infoErr = avformat_find_stream_info(fmt, nullptr);
  if (infoErr < 0) {
    std::string msg = "Failed to read stream info: " + ffmpegError(infoErr);
    avformat_close_input(&fmt);
    setError(error, msg.c_str());
    return false;
  }

  const AVCodec* codec = nullptr;
  int streamIndex =
      av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
  if (streamIndex < 0 || !codec) {
    avformat_close_input(&fmt);
    setError(error, "No video stream found.");
    return false;
  }

  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if (!ctx) {
    avformat_close_input(&fmt);
    setError(error, "Failed to allocate video decoder.");
    return false;
  }
  if (avcodec_parameters_to_context(ctx,
                                    fmt->streams[streamIndex]->codecpar) < 0) {
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    setError(error, "Failed to configure video decoder.");
    return false;
  }

  ctx->thread_count = std::max(1, ctx->thread_count);
  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    setError(error, "Failed to open video decoder.");
    return false;
  }

  AVFrame* frame = av_frame_alloc();
  AVFrame* scratch = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();
  if (!frame || !scratch || !packet) {
    av_frame_free(&frame);
    av_frame_free(&scratch);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    setError(error, "Failed to allocate video buffers.");
    return false;
  }

  Impl* impl = new Impl();
  impl->fmt = fmt;
  impl->codec = ctx;
  impl->frame = frame;
  impl->scratch = scratch;
  impl->packet = packet;
  impl->streamIndex = streamIndex;
  impl->timeBase = fmt->streams[streamIndex]->time_base;
  impl->width = ctx->width;
  impl->height = ctx->height;
  impl->targetW = impl->width;
  impl->targetH = impl->height;
  impl->fullRange = mapFullRange(ctx->color_range);
  impl->yuvMatrix = mapColorMatrix(ctx->colorspace);
  impl->formatStartUs =
      (fmt->start_time != AV_NOPTS_VALUE) ? fmt->start_time : 0;
  if (fmt->streams[streamIndex]->start_time != AV_NOPTS_VALUE) {
    impl->streamStartUs =
        av_rescale_q(fmt->streams[streamIndex]->start_time,
                     impl->timeBase, AVRational{1, AV_TIME_BASE});
  }
  if (fmt->duration > 0) {
    impl->duration100ns = fmt->duration * 10;
  }
  impl_ = impl;
  return true;
}

void VideoDecoder::uninit() {
  if (!impl_) return;
  if (impl_->sws) {
    sws_freeContext(impl_->sws);
    impl_->sws = nullptr;
  }
  av_packet_free(&impl_->packet);
  av_frame_free(&impl_->frame);
  av_frame_free(&impl_->scratch);
  if (impl_->codec) {
    avcodec_free_context(&impl_->codec);
  }
  if (impl_->fmt) {
    avformat_close_input(&impl_->fmt);
  }
  delete impl_;
  impl_ = nullptr;
}

bool VideoDecoder::setTargetSize(int targetWidth, int targetHeight,
                                 std::string* error) {
  if (!impl_ || !impl_->codec) {
    setError(error, "Video decoder is not initialized.");
    return false;
  }
  if (targetWidth <= 0 || targetHeight <= 0) {
    setError(error, "Invalid target size for video decoding.");
    return false;
  }
  impl_->targetW = targetWidth;
  impl_->targetH = targetHeight;
  if (impl_->sws) {
    sws_freeContext(impl_->sws);
    impl_->sws = nullptr;
  }
  return true;
}

void VideoDecoder::flush() {
  if (!impl_ || !impl_->codec) return;
  avcodec_flush_buffers(impl_->codec);
  impl_->atEnd = false;
  impl_->eof = false;
}

bool VideoDecoder::readFrame(VideoFrame& out, VideoReadInfo* info,
                             bool decodePixels) {
  if (!impl_ || impl_->atEnd) return false;

  if (info) {
    *info = VideoReadInfo{};
  }

  auto emitFrame = [&](AVFrame* src) -> bool {
    int64_t bestPts = src->best_effort_timestamp;
    if (bestPts == AV_NOPTS_VALUE) {
      bestPts = src->pts;
    }
    int64_t ts100ns = 0;
    if (bestPts != AV_NOPTS_VALUE) {
      int64_t absUs =
          av_rescale_q(bestPts, impl_->timeBase, AVRational{1, AV_TIME_BASE});
      int64_t relUs = absUs - impl_->formatStartUs;
      if (relUs < 0) relUs = 0;
      ts100ns = relUs * 10;
    }
    out.width = impl_->targetW;
    out.height = impl_->targetH;
    out.timestamp100ns = ts100ns;
    out.fullRange = impl_->fullRange;
    out.yuvMatrix = impl_->yuvMatrix;

    if (!decodePixels) {
      out.format = VideoPixelFormat::Unknown;
      out.stride = 0;
      out.planeHeight = 0;
      out.rgba.clear();
      out.yuv.clear();
      if (info) {
        info->timestamp100ns = ts100ns;
      }
      return true;
    }

    int dstW = impl_->targetW;
    int dstH = impl_->targetH;
    int stride = std::max(2, dstW);
    if (stride & 1) ++stride;
    int planeHeight = dstH;
    size_t required = static_cast<size_t>(stride) *
                      static_cast<size_t>(planeHeight) * 3u / 2u;
    try {
      out.yuv.resize(required);
    } catch (const std::bad_alloc&) {
      impl_->atEnd = true;
      return false;
    }
    out.rgba.clear();
    out.format = VideoPixelFormat::NV12;
    out.stride = stride;
    out.planeHeight = planeHeight;

    uint8_t* yPlane = out.yuv.data();
    uint8_t* uvPlane = yPlane + static_cast<size_t>(stride) *
                                   static_cast<size_t>(planeHeight);
    uint8_t* dstData[4] = {yPlane, uvPlane, nullptr, nullptr};
    int dstLinesize[4] = {stride, stride, 0, 0};

    AVPixelFormat srcFmt = static_cast<AVPixelFormat>(src->format);
    if (srcFmt == AV_PIX_FMT_NV12 && src->width == dstW &&
        src->height == dstH) {
      for (int y = 0; y < dstH; ++y) {
        std::memcpy(yPlane + y * stride, src->data[0] + y * src->linesize[0],
                    static_cast<size_t>(dstW));
      }
      int uvH = dstH / 2;
      for (int y = 0; y < uvH; ++y) {
        std::memcpy(uvPlane + y * stride, src->data[1] + y * src->linesize[1],
                    static_cast<size_t>(dstW));
      }
    } else {
      impl_->sws = sws_getCachedContext(
          impl_->sws, src->width, src->height, srcFmt, dstW, dstH,
          AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (!impl_->sws) {
        impl_->atEnd = true;
        return false;
      }
      sws_scale(impl_->sws, src->data, src->linesize, 0, src->height,
                dstData, dstLinesize);
    }

    if (info) {
      info->timestamp100ns = ts100ns;
      info->duration100ns = 0;
    }
    return true;
  };

  while (true) {
    int recv = avcodec_receive_frame(impl_->codec, impl_->frame);
    if (recv == 0) {
      return emitFrame(impl_->frame);
    }
    if (recv == AVERROR_EOF) {
      impl_->atEnd = true;
      return false;
    }
    if (recv != AVERROR(EAGAIN)) {
      impl_->atEnd = true;
      return false;
    }

    if (!impl_->eof) {
      int read = av_read_frame(impl_->fmt, impl_->packet);
      if (read < 0) {
        impl_->eof = true;
        avcodec_send_packet(impl_->codec, nullptr);
        continue;
      }
      if (impl_->packet->stream_index != impl_->streamIndex) {
        av_packet_unref(impl_->packet);
        continue;
      }
      avcodec_send_packet(impl_->codec, impl_->packet);
      av_packet_unref(impl_->packet);
    }
  }
}

bool VideoDecoder::seekToTimestamp100ns(int64_t timestamp100ns) {
  if (!impl_ || !impl_->fmt) return false;
  if (impl_->streamIndex < 0) return false;
  int64_t targetUs = timestamp100ns / 10;
  targetUs += impl_->formatStartUs;
  if (targetUs < 0) targetUs = 0;
  int64_t target =
      av_rescale_q(targetUs, AVRational{1, AV_TIME_BASE}, impl_->timeBase);
  int res = av_seek_frame(impl_->fmt, impl_->streamIndex, target,
                          AVSEEK_FLAG_BACKWARD);
  if (res < 0) return false;
  avcodec_flush_buffers(impl_->codec);
  impl_->atEnd = false;
  impl_->eof = false;
  return true;
}

bool VideoDecoder::atEnd() const { return impl_ ? impl_->atEnd : true; }
int VideoDecoder::width() const { return impl_ ? impl_->width : 0; }
int VideoDecoder::height() const { return impl_ ? impl_->height : 0; }
int64_t VideoDecoder::duration100ns() const {
  return impl_ ? impl_->duration100ns : 0;
}
