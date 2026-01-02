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
#include <libavcodec/codec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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

constexpr int kMaxDecodeWidth = 1024;
constexpr int kMaxDecodeHeight = 768;

void clampTargetSize(int& width, int& height) {
  width = std::max(2, width);
  height = std::max(4, height);
  if (width & 1) ++width;
  if (height & 1) ++height;
  if (width <= kMaxDecodeWidth && height <= kMaxDecodeHeight) return;
  double scaleW = static_cast<double>(kMaxDecodeWidth) / width;
  double scaleH = static_cast<double>(kMaxDecodeHeight) / height;
  double scale = std::min(scaleW, scaleH);
  width = static_cast<int>(std::lround(width * scale));
  height = static_cast<int>(std::lround(height * scale));
  width = std::min(width, kMaxDecodeWidth);
  height = std::min(height, kMaxDecodeHeight);
  width &= ~1;
  height &= ~1;
  width = std::max(2, width);
  height = std::max(4, height);
}

static enum AVPixelFormat get_hw_format(AVCodecContext* ctx,
                                        const enum AVPixelFormat* pix_fmts) {
  const enum AVPixelFormat* p;
  for (p = pix_fmts; *p != -1; p++) {
    if (*p == AV_PIX_FMT_D3D11) return *p;
  }
  return avcodec_default_get_format(ctx, pix_fmts);
}
}  // namespace

struct VideoDecoder::Impl {
  AVFormatContext* fmt = nullptr;
  AVCodecContext* codec = nullptr;
  AVBufferRef* hw_device_ctx = nullptr;
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

  bool emitFrame(VideoFrame& out, VideoReadInfo* info, bool decodePixels,
                 AVFrame* src) {
    int64_t bestPts = src->best_effort_timestamp;
    if (bestPts == AV_NOPTS_VALUE) {
      bestPts = src->pts;
    }
    int64_t ts100ns = 0;
    if (bestPts != AV_NOPTS_VALUE) {
      int64_t absUs =
          av_rescale_q(bestPts, timeBase, AVRational{1, AV_TIME_BASE});
      int64_t relUs = absUs - formatStartUs;
      if (relUs < 0) relUs = 0;
      ts100ns = relUs * 10;
    }
    out.width = targetW;
    out.height = targetH;
    out.timestamp100ns = ts100ns;
    out.fullRange = fullRange;
    out.yuvMatrix = yuvMatrix;

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

    int dstW = targetW;
    int dstH = targetH;
    int stride = std::max(2, dstW);
    if (stride & 1) ++stride;
    int planeHeight = dstH;
    size_t required = static_cast<size_t>(stride) *
                      static_cast<size_t>(planeHeight) * 3u / 2u;
    try {
      out.yuv.resize(required);
    } catch (const std::bad_alloc&) {
      atEnd = true;
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
      // Use point sampling for high-res content (like 4K) to avoid excessive CPU usage.
      // Bilinear scaling requires reading all source pixels, which is too slow for 4K on CPU.
      int flags = SWS_FAST_BILINEAR;
      if (static_cast<int64_t>(src->width) * src->height > 2560 * 1440) {
        flags = SWS_POINT;
      }

      sws = sws_getCachedContext(
          sws, src->width, src->height, srcFmt, dstW, dstH, AV_PIX_FMT_NV12,
          flags, nullptr, nullptr, nullptr);
      if (!sws) {
        atEnd = true;
        return false;
      }
      sws_scale(sws, src->data, src->linesize, 0, src->height, dstData,
                dstLinesize);
    }

    if (info) {
      info->timestamp100ns = ts100ns;
      info->duration100ns = 0;
    }
    return true;
  }
};

VideoDecoder::~VideoDecoder() { uninit(); }

bool VideoDecoder::init(const std::filesystem::path& path, std::string* error,
                        bool preferHardware, bool allowRgbOutput) {
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

  // Let FFmpeg choose an appropriate thread count unless explicitly set.
  if (ctx->thread_count < 0) {
    ctx->thread_count = 0;
  }

  AVBufferRef* hw_device_ctx = nullptr;
  if (preferHardware) {
    if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_D3D11VA,
                               nullptr, nullptr, 0) >= 0) {
      ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
      ctx->get_format = get_hw_format;
      ctx->extra_hw_frames = 16;
    }
  }

  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    setError(error, "Failed to open video decoder.");
    return false;
  }

  AVFrame* frame = av_frame_alloc();
  AVFrame* scratch = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();
  if (!frame || !scratch || !packet) {
    if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
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
  impl->hw_device_ctx = hw_device_ctx;
  impl->frame = frame;
  impl->scratch = scratch;
  impl->packet = packet;
  impl->streamIndex = streamIndex;
  impl->timeBase = fmt->streams[streamIndex]->time_base;
  impl->width = ctx->width;
  impl->height = ctx->height;
  impl->targetW = impl->width;
  impl->targetH = impl->height;
  clampTargetSize(impl->targetW, impl->targetH);
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
  if (impl_->hw_device_ctx) {
    av_buffer_unref(&impl_->hw_device_ctx);
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
  clampTargetSize(targetWidth, targetHeight);
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

  while (true) {
    int recv = avcodec_receive_frame(impl_->codec, impl_->frame);
    if (recv == 0) {
      AVFrame* src = impl_->frame;
      if (src->format == AV_PIX_FMT_D3D11) {
        if (decodePixels) {
          av_frame_unref(impl_->scratch);
          if (av_hwframe_transfer_data(impl_->scratch, src, 0) < 0) {
            // Try one more time with a fresh unref
            av_frame_unref(impl_->scratch);
            if (av_hwframe_transfer_data(impl_->scratch, src, 0) < 0) {
              // Transfer failed. Skip this frame and try next.
              continue;
            }
          }
          src = impl_->scratch;
          src->pts = impl_->frame->pts;
          src->best_effort_timestamp = impl_->frame->best_effort_timestamp;
        }
      }
      return impl_->emitFrame(out, info, decodePixels, src);
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

bool VideoDecoder::redecodeLastFrame(VideoFrame& out) {
  if (!impl_ || !impl_->frame) return false;
  return impl_->emitFrame(out, nullptr, true, impl_->frame);
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
