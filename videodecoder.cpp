#include "videodecoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

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

constexpr int kNoFrameTimeoutMs = 100;
constexpr int64_t kProbeSize = 64 * 1024;
constexpr int64_t kAnalyzeDurationFastUs = 500000;
constexpr int64_t kAnalyzeDurationFallbackUs = 5000000;

std::string ffmpegError(int err) {
  char buf[256];
  buf[0] = '\0';
  av_strerror(err, buf, sizeof(buf));
  return std::string(buf);
}

bool openInputWithProbe(const std::filesystem::path& path,
                        int64_t analyzeDurationUs,
                        AVFormatContext** outFmt,
                        std::string* error) {
  AVDictionary* options = nullptr;
  av_dict_set_int(&options, "probesize", kProbeSize, 0);
  av_dict_set_int(&options, "analyzeduration", analyzeDurationUs, 0);

  int openErr =
      avformat_open_input(outFmt, path.string().c_str(), nullptr, &options);
  av_dict_free(&options);
  if (openErr < 0) {
    std::string msg = "Failed to open video: " + ffmpegError(openErr);
    setError(error, msg.c_str());
    return false;
  }

  (*outFmt)->flags |= AVFMT_FLAG_NOBUFFER;
  (*outFmt)->max_analyze_duration = analyzeDurationUs;
  (*outFmt)->probesize = kProbeSize;
  return true;
}

YuvMatrix mapColorMatrix(AVColorSpace space) {
  switch (space) {
    case AVCOL_SPC_BT709:
      return YuvMatrix::Bt709;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
      return YuvMatrix::Bt2020;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
      return YuvMatrix::Bt601;
    default:
      return YuvMatrix::Bt709;
  }
}

bool mapFullRange(AVColorRange range) {
  return range == AVCOL_RANGE_JPEG;
}

YuvTransfer mapColorTransfer(AVColorTransferCharacteristic transfer) {
  switch (transfer) {
    case AVCOL_TRC_SMPTE2084:
      return YuvTransfer::Pq;
    case AVCOL_TRC_ARIB_STD_B67:
      return YuvTransfer::Hlg;
    default:
      return YuvTransfer::Sdr;
  }
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
  YuvMatrix yuvMatrix = YuvMatrix::Bt709;
  YuvTransfer yuvTransfer = YuvTransfer::Sdr;
  bool fullRange = true;
  int consecutiveTransferErrors = 0;
  bool hasPendingPacket = false;
  bool useSharedDevice = false;  // When true, keep frames on GPU without transfer
  uint64_t seekEpoch = 0;  // Incremented on seek to invalidate stale frames
  int framesAfterSeek = 0;  // Count frames since last seek for stabilization
  bool droppingNonKeyframes = false; // Drop frames until a keyframe is found

  bool emitFrame(VideoFrame& out, VideoReadInfo* info, bool decodePixels,
                 AVFrame* src, bool keepOnGpu = false) {
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
    AVColorSpace frameSpace = src->colorspace;
    if (frameSpace == AVCOL_SPC_UNSPECIFIED ||
        frameSpace == AVCOL_SPC_RESERVED) {
      frameSpace = codec ? codec->colorspace : frameSpace;
    }
    AVColorPrimaries framePrimaries = src->color_primaries;
    if (framePrimaries == AVCOL_PRI_UNSPECIFIED ||
        framePrimaries == AVCOL_PRI_RESERVED) {
      framePrimaries = codec ? codec->color_primaries : framePrimaries;
    }
    YuvMatrix frameMatrix = yuvMatrix;
    if (frameSpace != AVCOL_SPC_UNSPECIFIED &&
        frameSpace != AVCOL_SPC_RESERVED) {
      frameMatrix = mapColorMatrix(frameSpace);
    } else if (framePrimaries == AVCOL_PRI_BT2020) {
      frameMatrix = YuvMatrix::Bt2020;
    }
    AVColorTransferCharacteristic frameTrc = src->color_trc;
    if (frameTrc == AVCOL_TRC_UNSPECIFIED || frameTrc == AVCOL_TRC_RESERVED) {
      frameTrc = codec ? codec->color_trc : frameTrc;
    }
    YuvTransfer frameTransfer = yuvTransfer;
    if (frameTrc != AVCOL_TRC_UNSPECIFIED && frameTrc != AVCOL_TRC_RESERVED) {
      frameTransfer = mapColorTransfer(frameTrc);
    } else if (framePrimaries == AVCOL_PRI_BT2020 ||
               frameMatrix == YuvMatrix::Bt2020) {
      frameTransfer = YuvTransfer::Pq;
    }
    
    // For shared device mode, use native resolution (no CPU downscale)
    int outWidth = useSharedDevice ? src->width : targetW;
    int outHeight = useSharedDevice ? src->height : targetH;
    
    out.width = outWidth;
    out.height = outHeight;
    out.timestamp100ns = ts100ns;
    out.fullRange = fullRange;
    out.yuvMatrix = frameMatrix;
    out.yuvTransfer = frameTransfer;
    out.hwTexture.Reset();
    out.hwTextureArrayIndex = 0;

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
    
    // Zero-copy GPU path: pass the texture directly without CPU transfer
    if (keepOnGpu && src->format == AV_PIX_FMT_D3D11) {
      // D3D11VA frames have texture in data[0] and array index in data[1]
      auto* texture = reinterpret_cast<ID3D11Texture2D*>(src->data[0]);
      if (!texture) return false;
      intptr_t arrayIndex = reinterpret_cast<intptr_t>(src->data[1]);
      
      out.format = VideoPixelFormat::HWTexture;
      out.hwTexture = texture; // ComPtr assignment calls AddRef
      out.hwTextureArrayIndex = static_cast<int>(arrayIndex);
      out.stride = 0;
      out.planeHeight = 0;
      out.yuv.clear();
      out.rgba.clear();
      
      if (info) {
        info->timestamp100ns = ts100ns;
        info->duration100ns = 0;
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
  if (!openInputWithProbe(path, kAnalyzeDurationFastUs, &fmt, error)) {
    return false;
  }

  int infoErr = avformat_find_stream_info(fmt, nullptr);
  if (infoErr < 0) {
    avformat_close_input(&fmt);
    if (!openInputWithProbe(path, kAnalyzeDurationFallbackUs, &fmt, error)) {
      return false;
    }
    infoErr = avformat_find_stream_info(fmt, nullptr);
    if (infoErr < 0) {
      std::string msg = "Failed to read stream info: " + ffmpegError(infoErr);
      avformat_close_input(&fmt);
      setError(error, msg.c_str());
      return false;
    }
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

  // Prefer recovering corrupted streams over stalling on missing references.
  ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
  ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
  ctx->err_recognition = AV_EF_IGNORE_ERR;

  // Additional error recovery for VP9/AV1 (common in HDR content)
  if (ctx->codec_id == AV_CODEC_ID_VP9 || ctx->codec_id == AV_CODEC_ID_AV1) {
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;  // Skip some checks for speed/error recovery
    ctx->err_recognition |= AV_EF_EXPLODE;  // Don't explode on errors, try to recover
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
      ctx->extra_hw_frames = 32;
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
  impl->yuvTransfer = mapColorTransfer(ctx->color_trc);
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

bool VideoDecoder::initWithDevice(const std::filesystem::path& path,
                                  ID3D11Device* device,
                                  std::string* error) {
  uninit();

  if (!device) {
    setError(error, "Invalid D3D11 device.");
    return false;
  }

  AVFormatContext* fmt = nullptr;
  if (!openInputWithProbe(path, kAnalyzeDurationFastUs, &fmt, error)) {
    return false;
  }

  int infoErr = avformat_find_stream_info(fmt, nullptr);
  if (infoErr < 0) {
    avformat_close_input(&fmt);
    if (!openInputWithProbe(path, kAnalyzeDurationFallbackUs, &fmt, error)) {
      return false;
    }
    infoErr = avformat_find_stream_info(fmt, nullptr);
    if (infoErr < 0) {
      std::string msg = "Failed to read stream info: " + ffmpegError(infoErr);
      avformat_close_input(&fmt);
      setError(error, msg.c_str());
      return false;
    }
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

  // Prefer recovering corrupted streams over stalling on missing references.
  ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
  ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
  ctx->err_recognition = AV_EF_IGNORE_ERR;

  if (ctx->thread_count < 0) {
    ctx->thread_count = 0;
  }

  // Create hw_device_ctx wrapping the external D3D11 device
  AVBufferRef* hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  if (!hw_device_ctx) {
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    setError(error, "Failed to allocate hardware device context.");
    return false;
  }

  AVHWDeviceContext* device_ctx = reinterpret_cast<AVHWDeviceContext*>(hw_device_ctx->data);
  AVD3D11VADeviceContext* d3d11_device_ctx = reinterpret_cast<AVD3D11VADeviceContext*>(device_ctx->hwctx);
  
  // Use the external device (add reference)
  device->AddRef();
  d3d11_device_ctx->device = device;
  
  // Set device context (FFmpeg will use the immediate context)
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediateContext;
  device->GetImmediateContext(&immediateContext);
  immediateContext->AddRef();
  d3d11_device_ctx->device_context = immediateContext.Get();
  
  // Let FFmpeg set up its own locking (it uses an internal mutex if unset)
  d3d11_device_ctx->lock = nullptr;
  d3d11_device_ctx->unlock = nullptr;
  d3d11_device_ctx->lock_ctx = nullptr;

  int initErr = av_hwdevice_ctx_init(hw_device_ctx);
  if (initErr < 0) {
    av_buffer_unref(&hw_device_ctx);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    std::string msg = "Failed to init hw device context: " + ffmpegError(initErr);
    setError(error, msg.c_str());
    return false;
  }

  ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
  ctx->get_format = get_hw_format;
  ctx->extra_hw_frames = 32;

  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    av_buffer_unref(&hw_device_ctx);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    setError(error, "Failed to open video decoder.");
    return false;
  }

  AVFrame* frame = av_frame_alloc();
  AVFrame* scratch = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();
  if (!frame || !scratch || !packet) {
    av_buffer_unref(&hw_device_ctx);
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
  // Don't clamp for shared device mode - use native resolution
  impl->fullRange = mapFullRange(ctx->color_range);
  impl->yuvMatrix = mapColorMatrix(ctx->colorspace);
  impl->yuvTransfer = mapColorTransfer(ctx->color_trc);
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
  impl->useSharedDevice = true;  // Enable zero-copy GPU path
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
  if (impl_->packet) {
    av_packet_unref(impl_->packet);
    av_packet_free(&impl_->packet);
  }
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
  const auto start = std::chrono::steady_clock::now();

  while (true) {
    int recv = avcodec_receive_frame(impl_->codec, impl_->frame);
    if (recv == 0) {
      AVFrame* src = impl_->frame;
      
      // VLC-style robustness: After seek, drop everything until we hit a real keyframe.
      // This hides the "searching" artifacts and ensures we start on a valid frame.
      if (impl_->droppingNonKeyframes) {
        bool isKeyframe = (src->flags & AV_FRAME_FLAG_KEY) || (src->pict_type == AV_PICTURE_TYPE_I);
        if (!isKeyframe) {
           impl_->framesAfterSeek++;
           // Safety valve: if we don't find a keyframe after 60 frames (~2s), give up and show whatever.
           if (impl_->framesAfterSeek < 60) {
             av_frame_unref(src);
             continue;
           }
           impl_->droppingNonKeyframes = false;
        } else {
           impl_->droppingNonKeyframes = false;
        }
      }
      
      if (src->format == AV_PIX_FMT_D3D11) {
        if (decodePixels) {
          // Zero-copy path: keep frame on GPU when using shared device
          if (impl_->useSharedDevice) {
            // Pass the GPU frame directly without CPU transfer
            return impl_->emitFrame(out, info, decodePixels, src, true);
          }
          
          // Legacy path: transfer to CPU
          av_frame_unref(impl_->scratch);
          if (av_hwframe_transfer_data(impl_->scratch, src, 0) < 0) {
            // Try one more time with a fresh unref
            av_frame_unref(impl_->scratch);
            if (av_hwframe_transfer_data(impl_->scratch, src, 0) < 0) {
              // Transfer failed. Skip this frame and try next.
              impl_->consecutiveTransferErrors++;
              if (impl_->consecutiveTransferErrors > 10) {
                // Too many failures, assume device lost or bad state.
                // Return false without setting atEnd to trigger error/fallback.
                return false;
              }
              continue;
            }
          }
          impl_->consecutiveTransferErrors = 0;
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

    if (impl_->eof) {
      impl_->atEnd = true;
      return false;
    }

    if (!impl_->hasPendingPacket) {
      int read = av_read_frame(impl_->fmt, impl_->packet);
      if (read < 0) {
        impl_->eof = true;
        avcodec_send_packet(impl_->codec, nullptr);
        continue;
      }
      impl_->hasPendingPacket = true;
    }

    if (impl_->hasPendingPacket) {
      if (impl_->packet->stream_index != impl_->streamIndex) {
        av_packet_unref(impl_->packet);
        impl_->hasPendingPacket = false;
        continue;
      }

      int send = avcodec_send_packet(impl_->codec, impl_->packet);
      if (send == AVERROR(EAGAIN)) {
        // Decoder is full, but receive returned EAGAIN.
        // This implies a deadlock or unexpected state.
        // We cannot send, and we cannot receive.
        // This is a fatal error for this decoder instance.
        return false;
      }

      av_packet_unref(impl_->packet);
      impl_->hasPendingPacket = false;

      if (send < 0 && send != AVERROR_EOF) {
        // Error sending packet (e.g. invalid data).
        // We dropped the packet and continue to next one.
        continue;
      }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (elapsed.count() >= kNoFrameTimeoutMs) {
      if (info) {
        info->noFrameTimeoutMs =
            static_cast<uint32_t>(std::min<int64_t>(elapsed.count(), UINT32_MAX));
      }
      return false;
    }
  }
}

bool VideoDecoder::redecodeLastFrame(VideoFrame& out) {
  if (!impl_ || !impl_->frame) return false;

  AVFrame* src = impl_->frame;
  if (src->format == AV_PIX_FMT_D3D11) {
    av_frame_unref(impl_->scratch);
    if (av_hwframe_transfer_data(impl_->scratch, src, 0) < 0) {
      // Try one more time with a fresh unref
      av_frame_unref(impl_->scratch);
      if (av_hwframe_transfer_data(impl_->scratch, src, 0) < 0) {
        return false;
      }
    }
    src = impl_->scratch;
    src->pts = impl_->frame->pts;
    src->best_effort_timestamp = impl_->frame->best_effort_timestamp;
  }

  return impl_->emitFrame(out, nullptr, true, src);
}

bool VideoDecoder::seekToTimestamp100ns(int64_t timestamp100ns) {
  if (!impl_ || !impl_->fmt) return false;
  if (impl_->streamIndex < 0) return false;

  if (impl_->packet) {
    av_packet_unref(impl_->packet);
  }
  impl_->hasPendingPacket = false;

  int64_t targetUs = timestamp100ns / 10;
  targetUs += impl_->formatStartUs;
  if (targetUs < 0) targetUs = 0;

  // Prefer seeking to keyframes for codecs like VP9 that have complex frame dependencies
  // This prevents crashes from missing reference frames during HDR playback
  int seekFlags = AVSEEK_FLAG_BACKWARD;
  if (impl_->codec && (impl_->codec->codec_id == AV_CODEC_ID_VP9 || 
                       impl_->codec->codec_id == AV_CODEC_ID_AV1)) {
    seekFlags |= AVSEEK_FLAG_ANY;  // Allow seeking to any frame, but we'll refine below
  }

  // Try avformat_seek_file with stream_index = -1 (AV_TIME_BASE)
  int res = avformat_seek_file(impl_->fmt, -1, INT64_MIN, targetUs, INT64_MAX, seekFlags);

  if (res < 0) {
    // Fallback: try av_seek_frame with stream index (legacy method)
    int64_t targetStream = av_rescale_q(targetUs, AVRational{1, AV_TIME_BASE}, impl_->timeBase);
    res = av_seek_frame(impl_->fmt, impl_->streamIndex, targetStream, seekFlags);
  }

  // For VP9/AV1, ensure we seeked to a keyframe by reading forward until we find one
  if (res >= 0 && impl_->codec && (impl_->codec->codec_id == AV_CODEC_ID_VP9 || 
                                   impl_->codec->codec_id == AV_CODEC_ID_AV1)) {
    AVPacket* pkt = av_packet_alloc();
    if (pkt) {
      bool foundKeyframe = false;
      // Read packets until we find a keyframe or timeout
      for (int attempts = 0; attempts < 100 && !foundKeyframe; ++attempts) {
        res = av_read_frame(impl_->fmt, pkt);
        if (res < 0) break;
        if (pkt->stream_index == impl_->streamIndex && (pkt->flags & AV_PKT_FLAG_KEY)) {
          foundKeyframe = true;
        }
        av_packet_unref(pkt);
      }
      av_packet_free(&pkt);
      if (!foundKeyframe) {
        // If no keyframe found, the seek might still work, but log it
        // For now, continue - the decoder has error recovery flags
      }
    }
  }

  if (res < 0) return false;

  avcodec_flush_buffers(impl_->codec);
  impl_->atEnd = false;
  impl_->eof = false;
  impl_->consecutiveTransferErrors = 0;
  impl_->seekEpoch++;
  impl_->framesAfterSeek = 0;
  impl_->droppingNonKeyframes = true;
  return true;
}

bool VideoDecoder::atEnd() const { return impl_ ? impl_->atEnd : true; }
int VideoDecoder::width() const { return impl_ ? impl_->width : 0; }
int VideoDecoder::height() const { return impl_ ? impl_->height : 0; }
int64_t VideoDecoder::duration100ns() const {
  return impl_ ? impl_->duration100ns : 0;
}
