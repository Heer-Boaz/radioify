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
#include <libavutil/pixdesc.h>
#include <libavutil/display.h>
#include <libavutil/version.h>
#include <libswscale/swscale.h>
}

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <new>

// C-style callbacks for FFmpeg locking
static void d3d11_lock(void* ctx) {
    if (ctx) reinterpret_cast<std::recursive_mutex*>(ctx)->lock();
}

static void d3d11_unlock(void* ctx) {
    if (ctx) reinterpret_cast<std::recursive_mutex*>(ctx)->unlock();
}

namespace {
void setError(std::string* error, const char* message) {
  if (error) *error = message;
}

int64_t frameDurationTicks(const AVFrame* frame) {
  if (!frame) return 0;
#if LIBAVUTIL_VERSION_MAJOR >= 59
  return frame->duration;
#else
  if (frame->duration > 0) return frame->duration;
  return frame->pkt_duration;
#endif
}

// Upper bound for a single readFrame() call. This is used to keep the decode
// thread responsive to commands (seek/resize) without treating transient stalls
// as fatal.
constexpr int kNoFrameTimeoutMs = 1000;
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

AVPixelFormat chooseD3d11SwFormat(const AVCodecContext* ctx) {
  if (!ctx) return AV_PIX_FMT_NV12;
  int bits = ctx->bits_per_raw_sample;
  if (bits <= 0) {
    bits = ctx->bits_per_coded_sample;
  }
  if (bits <= 0) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(ctx->pix_fmt);
    if (desc) {
      bits = desc->comp[0].depth;
    }
  }
  return (bits > 8) ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
}

bool configureD3d11Frames(AVCodecContext* ctx, AVBufferRef* hwDeviceCtx,
                          bool shaderResource) {
  if (!ctx || !hwDeviceCtx) return false;
  AVBufferRef* framesRef = av_hwframe_ctx_alloc(hwDeviceCtx);
  if (!framesRef) return false;

  AVHWFramesContext* framesCtx =
      reinterpret_cast<AVHWFramesContext*>(framesRef->data);
  framesCtx->format = AV_PIX_FMT_D3D11;
  framesCtx->sw_format = chooseD3d11SwFormat(ctx);
  framesCtx->width = (ctx->coded_width > 0) ? ctx->coded_width : ctx->width;
  framesCtx->height = (ctx->coded_height > 0) ? ctx->coded_height : ctx->height;
  framesCtx->initial_pool_size =
      (ctx->extra_hw_frames > 0) ? ctx->extra_hw_frames + 2 : 32;

  AVD3D11VAFramesContext* framesHw =
      reinterpret_cast<AVD3D11VAFramesContext*>(framesCtx->hwctx);
  framesHw->BindFlags = D3D11_BIND_DECODER;
  if (shaderResource) {
    framesHw->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
  }
  framesHw->MiscFlags = 0;

  if (av_hwframe_ctx_init(framesRef) < 0) {
    av_buffer_unref(&framesRef);
    return false;
  }

  ctx->hw_frames_ctx = av_buffer_ref(framesRef);
  av_buffer_unref(&framesRef);
  return true;
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

void normalizeEvenTargetSize(int& width, int& height) {
  width = std::max(2, width);
  height = std::max(2, height);
  if (width & 1) ++width;
  if (height & 1) ++height;
}

int normalizeQuarterTurns(int quarterTurns) {
  quarterTurns %= 4;
  if (quarterTurns < 0) quarterTurns += 4;
  return quarterTurns;
}

void applyRotationToDimensions(int inWidth, int inHeight, int quarterTurns,
                               int* outWidth, int* outHeight) {
  if (!outWidth || !outHeight) return;
  quarterTurns = normalizeQuarterTurns(quarterTurns);
  if ((quarterTurns & 1) != 0) {
    *outWidth = inHeight;
    *outHeight = inWidth;
  } else {
    *outWidth = inWidth;
    *outHeight = inHeight;
  }
}

int streamRotationQuarterTurns(AVStream* stream) {
  if (!stream) return 0;

  double theta = 0.0;
  AVDictionaryEntry* rotateTag =
      av_dict_get(stream->metadata, "rotate", nullptr, 0);
  if (rotateTag && rotateTag->value && rotateTag->value[0] != '\0') {
    theta = std::strtod(rotateTag->value, nullptr);
  }

  size_t displayMatrixSize = 0;
  const uint8_t* displayMatrix =
      av_stream_get_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX,
                              &displayMatrixSize);
  if (displayMatrix &&
      displayMatrixSize >= sizeof(int32_t) * 9u) {
    // FFmpeg stores matrix rotation in the opposite sign for display usage.
    const double matrixTheta = -av_display_rotation_get(
        reinterpret_cast<const int32_t*>(displayMatrix));
    if (std::isfinite(matrixTheta)) {
      theta = matrixTheta;
    }
  }

  if (!std::isfinite(theta)) return 0;
  theta = std::fmod(theta, 360.0);
  if (theta < 0.0) theta += 360.0;

  const double snapped = std::round(theta / 90.0) * 90.0;
  if (std::fabs(theta - snapped) > 2.0) {
    return 0;
  }
  theta = snapped;

  const int quarterTurns = static_cast<int>(std::lround(theta / 90.0));
  return normalizeQuarterTurns(quarterTurns);
}

bool rotateNv12(const uint8_t* src, int srcStride, int srcPlaneHeight, int srcW,
                int srcH, int quarterTurns, uint8_t* dst, int dstStride,
                int dstPlaneHeight, int dstW, int dstH) {
  if (!src || !dst) return false;
  if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return false;
  if (srcStride < srcW || dstStride < dstW) return false;

  quarterTurns = normalizeQuarterTurns(quarterTurns);
  int expectedW = 0;
  int expectedH = 0;
  applyRotationToDimensions(srcW, srcH, quarterTurns, &expectedW, &expectedH);
  if (dstW != expectedW || dstH != expectedH) return false;

  const uint8_t* srcY = src;
  const uint8_t* srcUV =
      src + static_cast<size_t>(srcStride) * static_cast<size_t>(srcPlaneHeight);
  uint8_t* dstY = dst;
  uint8_t* dstUV =
      dst + static_cast<size_t>(dstStride) * static_cast<size_t>(dstPlaneHeight);

  if (quarterTurns == 0) {
    for (int y = 0; y < srcH; ++y) {
      std::memcpy(dstY + y * dstStride, srcY + y * srcStride,
                  static_cast<size_t>(srcW));
    }
    const int srcChromaH = srcH / 2;
    for (int y = 0; y < srcChromaH; ++y) {
      std::memcpy(dstUV + y * dstStride, srcUV + y * srcStride,
                  static_cast<size_t>(srcW));
    }
    return true;
  }

  for (int y = 0; y < dstH; ++y) {
    for (int x = 0; x < dstW; ++x) {
      int sx = 0;
      int sy = 0;
      switch (quarterTurns) {
        case 1:  // 90 CW
          sx = y;
          sy = srcH - 1 - x;
          break;
        case 2:  // 180
          sx = srcW - 1 - x;
          sy = srcH - 1 - y;
          break;
        case 3:  // 270 CW
          sx = srcW - 1 - y;
          sy = x;
          break;
        default:
          return false;
      }
      dstY[y * dstStride + x] = srcY[sy * srcStride + sx];
    }
  }

  const int srcChromaW = srcW / 2;
  const int srcChromaH = srcH / 2;
  const int dstChromaW = dstW / 2;
  const int dstChromaH = dstH / 2;
  for (int y = 0; y < dstChromaH; ++y) {
    for (int x = 0; x < dstChromaW; ++x) {
      int sx = 0;
      int sy = 0;
      switch (quarterTurns) {
        case 1:  // 90 CW
          sx = y;
          sy = srcChromaH - 1 - x;
          break;
        case 2:  // 180
          sx = srcChromaW - 1 - x;
          sy = srcChromaH - 1 - y;
          break;
        case 3:  // 270 CW
          sx = srcChromaW - 1 - y;
          sy = x;
          break;
        default:
          return false;
      }
      const uint8_t* srcPx = srcUV + sy * srcStride + sx * 2;
      uint8_t* dstPx = dstUV + y * dstStride + x * 2;
      dstPx[0] = srcPx[0];
      dstPx[1] = srcPx[1];
    }
  }
  return true;
}

struct StreamSelectionResult {
  int index = -1;
  const AVCodec* codec = nullptr;
};

bool selectHighestResolutionStream(AVFormatContext* fmt,
                                   StreamSelectionResult* out,
                                   VideoStreamSelection* selection,
                                   std::string* error) {
  if (!fmt || !out) {
    setError(error, "Invalid stream selection state.");
    return false;
  }
  if (selection) {
    selection->streams.clear();
    selection->selectedIndex = -1;
  }

  int bestIndex = -1;
  const AVCodec* bestCodec = nullptr;
  int64_t bestPixels = -1;
  int64_t bestBitRate = -1;
  bool bestDefault = false;

  for (unsigned int i = 0; i < fmt->nb_streams; ++i) {
    AVStream* stream = fmt->streams[i];
    if (!stream || !stream->codecpar) continue;
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;

    VideoStreamInfo info;
    info.index = static_cast<int>(i);
    info.width = stream->codecpar->width;
    info.height = stream->codecpar->height;
    info.bitRate =
        (stream->codecpar->bit_rate > 0) ? stream->codecpar->bit_rate : 0;
    info.isDefault = (stream->disposition & AV_DISPOSITION_DEFAULT) != 0;
    info.isAttachedPic =
        (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    info.hasDecoder = (codec != nullptr);
    if (codec) {
      info.codecName = avcodec_get_name(stream->codecpar->codec_id);
    }
    if (selection) {
      selection->streams.push_back(info);
    }

    if (info.isAttachedPic || !info.hasDecoder) continue;

    int64_t pixels =
        static_cast<int64_t>(info.width) * static_cast<int64_t>(info.height);
    if (pixels < 0) pixels = 0;
    bool pick = false;
    if (pixels > bestPixels) {
      pick = true;
    } else if (pixels == bestPixels) {
      if (info.isDefault && !bestDefault) {
        pick = true;
      } else if (info.isDefault == bestDefault && info.bitRate > bestBitRate) {
        pick = true;
      } else if (info.isDefault == bestDefault &&
                 info.bitRate == bestBitRate && bestIndex < 0) {
        pick = true;
      }
    }
    if (pick) {
      bestIndex = info.index;
      bestCodec = codec;
      bestPixels = pixels;
      bestBitRate = info.bitRate;
      bestDefault = info.isDefault;
    }
  }

  if (bestIndex < 0 || !bestCodec) {
    const AVCodec* fallbackCodec = nullptr;
    int fallbackIndex =
        av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &fallbackCodec, 0);
    if (fallbackIndex < 0 || !fallbackCodec) {
      setError(error, "No video stream found.");
      return false;
    }
    bestIndex = fallbackIndex;
    bestCodec = fallbackCodec;
  }

  out->index = bestIndex;
  out->codec = bestCodec;
  if (selection) {
    selection->selectedIndex = bestIndex;
  }
  return true;
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
  AVFrame* lastFrame = nullptr;
  AVFrame* scratch = nullptr;
  AVPacket* packet = nullptr;
  SwsContext* sws = nullptr;
  int streamIndex = -1;
  AVRational timeBase{0, 1};
  int width = 0;
  int height = 0;
  int targetW = 0;
  int targetH = 0;
  int rotationQuarterTurns = 0;
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
    int64_t duration100ns = 0;
    if (bestPts != AV_NOPTS_VALUE) {
      int64_t absUs =
          av_rescale_q(bestPts, timeBase, AVRational{1, AV_TIME_BASE});
      int64_t relUs = absUs - formatStartUs;
      if (relUs < 0) relUs = 0;
      ts100ns = relUs * 10;
    }
    int64_t frameDuration = frameDurationTicks(src);
    if (frameDuration > 0) {
      int64_t durUs =
          av_rescale_q(frameDuration, timeBase, AVRational{1, AV_TIME_BASE});
      if (durUs > 0) {
        duration100ns = durUs * 10;
      }
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

    const int rotationTurns = normalizeQuarterTurns(rotationQuarterTurns);
    const bool canKeepOnGpu = keepOnGpu && src->format == AV_PIX_FMT_D3D11;
    int outWidth = canKeepOnGpu ? src->width : targetW;
    int outHeight = canKeepOnGpu ? src->height : targetH;
    
    out.width = outWidth;
    out.height = outHeight;
    out.timestamp100ns = ts100ns;
    out.fullRange = fullRange;
    out.yuvMatrix = frameMatrix;
    out.yuvTransfer = frameTransfer;
    out.rotationQuarterTurns = 0;
    out.hwTexture.Reset();
    out.hwTextureArrayIndex = 0;
    out.hwFrameRef.reset();

    if (!decodePixels) {
      out.format = VideoPixelFormat::Unknown;
      out.stride = 0;
      out.planeHeight = 0;
      out.rgba.clear();
      out.yuv.clear();
      if (info) {
        info->timestamp100ns = ts100ns;
        info->duration100ns = duration100ns;
      }
      return true;
    }
    // Zero-copy GPU path: pass the texture directly without CPU transfer.
    if (canKeepOnGpu) {
      // D3D11VA frames have texture in data[0] and array index in data[1]
      auto* texture = reinterpret_cast<ID3D11Texture2D*>(src->data[0]);
      if (!texture) return false;
      intptr_t arrayIndex = reinterpret_cast<intptr_t>(src->data[1]);
      AVFrame* copy = av_frame_alloc();
      if (!copy) return false;
      if (av_frame_ref(copy, src) < 0) {
        av_frame_free(&copy);
        return false;
      }

      out.format = VideoPixelFormat::HWTexture;
      out.hwTexture = texture; // ComPtr assignment calls AddRef
      out.hwTextureArrayIndex = static_cast<int>(arrayIndex);
      out.hwFrameRef =
          std::shared_ptr<AVFrame>(copy, [](AVFrame* f) { av_frame_free(&f); });
      out.rotationQuarterTurns = rotationTurns;
      out.stride = 0;
      out.planeHeight = 0;
      out.yuv.clear();
      out.rgba.clear();
      
      if (info) {
        info->timestamp100ns = ts100ns;
        info->duration100ns = duration100ns;
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

    int scaledW = dstW;
    int scaledH = dstH;
    if ((rotationTurns & 1) != 0) {
      std::swap(scaledW, scaledH);
    }
    int scaledStride = std::max(2, scaledW);
    if (scaledStride & 1) ++scaledStride;
    int scaledPlaneHeight = scaledH;

    std::vector<uint8_t> scaled;
    uint8_t* scaledY = out.yuv.data();
    uint8_t* scaledUV =
        scaledY + static_cast<size_t>(stride) * static_cast<size_t>(planeHeight);
    if (rotationTurns != 0) {
      size_t scaledRequired = static_cast<size_t>(scaledStride) *
                              static_cast<size_t>(scaledPlaneHeight) * 3u / 2u;
      try {
        scaled.resize(scaledRequired);
      } catch (const std::bad_alloc&) {
        atEnd = true;
        return false;
      }
      scaledY = scaled.data();
      scaledUV = scaledY + static_cast<size_t>(scaledStride) *
                           static_cast<size_t>(scaledPlaneHeight);
    }

    uint8_t* dstData[4] = {scaledY, scaledUV, nullptr, nullptr};
    int dstLinesize[4] = {scaledStride, scaledStride, 0, 0};

    AVPixelFormat srcFmt = static_cast<AVPixelFormat>(src->format);
    if (srcFmt == AV_PIX_FMT_NV12 && src->width == scaledW &&
        src->height == scaledH) {
      for (int y = 0; y < scaledH; ++y) {
        std::memcpy(scaledY + y * scaledStride,
                    src->data[0] + y * src->linesize[0],
                    static_cast<size_t>(scaledW));
      }
      int uvH = scaledH / 2;
      for (int y = 0; y < uvH; ++y) {
        std::memcpy(scaledUV + y * scaledStride,
                    src->data[1] + y * src->linesize[1],
                    static_cast<size_t>(scaledW));
      }
    } else {
      // Use point sampling for high-res content (like 4K) to avoid excessive CPU usage.
      // Bilinear scaling requires reading all source pixels, which is too slow for 4K on CPU.
      int flags = SWS_FAST_BILINEAR;
      if (static_cast<int64_t>(src->width) * src->height > 2560 * 1440) {
        flags = SWS_POINT;
      }

      sws = sws_getCachedContext(
          sws, src->width, src->height, srcFmt, scaledW, scaledH,
          AV_PIX_FMT_NV12, flags, nullptr, nullptr, nullptr);
      if (!sws) {
        atEnd = true;
        return false;
      }
      sws_scale(sws, src->data, src->linesize, 0, src->height, dstData,
                dstLinesize);
    }

    if (rotationTurns != 0) {
      if (!rotateNv12(scaled.data(), scaledStride, scaledPlaneHeight, scaledW,
                      scaledH, rotationTurns, out.yuv.data(), stride,
                      planeHeight, dstW, dstH)) {
        atEnd = true;
        return false;
      }
    }

    if (info) {
      info->timestamp100ns = ts100ns;
      info->duration100ns = duration100ns;
    }
    return true;
  }
};

VideoDecoder::~VideoDecoder() { uninit(); }

bool VideoDecoder::init(const std::filesystem::path& path, std::string* error,
                        bool preferHardware, bool allowRgbOutput,
                        VideoStreamSelection* streamSelection) {
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

  StreamSelectionResult selectionResult;
  if (!selectHighestResolutionStream(fmt, &selectionResult, streamSelection,
                                     error)) {
    avformat_close_input(&fmt);
    return false;
  }
  const AVCodec* codec = selectionResult.codec;
  int streamIndex = selectionResult.index;
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
  AVFrame* lastFrame = av_frame_alloc();
  AVFrame* scratch = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();
  if (!frame || !lastFrame || !scratch || !packet) {
    if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
    av_frame_free(&frame);
    av_frame_free(&lastFrame);
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
  impl->lastFrame = lastFrame;
  impl->scratch = scratch;
  impl->packet = packet;
  impl->streamIndex = streamIndex;
  impl->timeBase = fmt->streams[streamIndex]->time_base;
  impl->rotationQuarterTurns =
      streamRotationQuarterTurns(fmt->streams[streamIndex]);
  applyRotationToDimensions(ctx->width, ctx->height,
                            impl->rotationQuarterTurns, &impl->width,
                            &impl->height);
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
                                  std::string* error,
                                  VideoStreamSelection* streamSelection,
                                  std::recursive_mutex* contextMutex) {
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

  StreamSelectionResult selectionResult;
  if (!selectHighestResolutionStream(fmt, &selectionResult, streamSelection,
                                     error)) {
    avformat_close_input(&fmt);
    return false;
  }
  const AVCodec* codec = selectionResult.codec;
  int streamIndex = selectionResult.index;
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
  
    // Set locking callbacks if a mutex is provided
  if (contextMutex) {
    d3d11_device_ctx->lock = d3d11_lock;
    d3d11_device_ctx->unlock = d3d11_unlock;
    d3d11_device_ctx->lock_ctx = contextMutex;
  } else {
    d3d11_device_ctx->lock = nullptr;
    d3d11_device_ctx->unlock = nullptr;
    d3d11_device_ctx->lock_ctx = nullptr;
  }


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
  configureD3d11Frames(ctx, ctx->hw_device_ctx, true);

  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    av_buffer_unref(&hw_device_ctx);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    setError(error, "Failed to open video decoder.");
    return false;
  }

  AVFrame* frame = av_frame_alloc();
  AVFrame* lastFrame = av_frame_alloc();
  AVFrame* scratch = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();
  if (!frame || !lastFrame || !scratch || !packet) {
    av_buffer_unref(&hw_device_ctx);
    av_frame_free(&frame);
    av_frame_free(&lastFrame);
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
  impl->lastFrame = lastFrame;
  impl->scratch = scratch;
  impl->packet = packet;
  impl->streamIndex = streamIndex;
  impl->timeBase = fmt->streams[streamIndex]->time_base;
  impl->rotationQuarterTurns =
      streamRotationQuarterTurns(fmt->streams[streamIndex]);
  applyRotationToDimensions(ctx->width, ctx->height,
                            impl->rotationQuarterTurns, &impl->width,
                            &impl->height);
  impl->targetW = impl->width;
  impl->targetH = impl->height;
  // Don't clamp for shared device mode - use native resolution
  if (impl->rotationQuarterTurns != 0) {
    // Shared-device playback rotates in GPU; keep fallback targets even.
    normalizeEvenTargetSize(impl->targetW, impl->targetH);
  }
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
  av_frame_free(&impl_->lastFrame);
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
  if (impl_->packet) {
    av_packet_unref(impl_->packet);
  }
  impl_->hasPendingPacket = false;
  if (impl_->frame) {
    av_frame_unref(impl_->frame);
  }
  if (impl_->lastFrame) {
    av_frame_unref(impl_->lastFrame);
  }
  if (impl_->scratch) {
    av_frame_unref(impl_->scratch);
  }
  impl_->atEnd = false;
  impl_->eof = false;
  impl_->consecutiveTransferErrors = 0;
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
      // After seek, skip a small number of potentially corrupt frames.
      // Don't rely on keyframe detection since HW decoders often don't set flags
      // correctly.
      if (impl_->framesAfterSeek < 3) {
        impl_->framesAfterSeek++;
        av_frame_unref(impl_->frame);
        continue;
      }

      // Keep a reference to the last decoded frame so redecodeLastFrame() works
      // even when readFrame() is called with decodePixels=false.
      av_frame_unref(impl_->lastFrame);
      if (av_frame_ref(impl_->lastFrame, impl_->frame) < 0) {
        av_frame_unref(impl_->frame);
        continue;
      }
      av_frame_unref(impl_->frame);

      AVFrame* src = impl_->lastFrame;
      if (src->format == AV_PIX_FMT_D3D11 && decodePixels) {
        // Zero-copy path: keep frame on GPU when using shared device.
        if (impl_->useSharedDevice) {
          return impl_->emitFrame(out, info, decodePixels, src, true);
        }

        // Transfer to CPU for standard fallback path.
        av_frame_unref(impl_->scratch);
        if (av_hwframe_transfer_data(impl_->scratch, src, 0) < 0) {
          // Try one more time with a fresh unref.
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
        impl_->scratch->pts = src->pts;
        impl_->scratch->best_effort_timestamp = src->best_effort_timestamp;
        src = impl_->scratch;
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
      if (send != AVERROR(EAGAIN)) {
        av_packet_unref(impl_->packet);
        impl_->hasPendingPacket = false;

        if (send < 0 && send != AVERROR_EOF) {
          // Error sending packet (e.g. invalid data).
          // We dropped the packet and continue to next one.
          continue;
        }
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
  if (!impl_ || !impl_->lastFrame) return false;

  AVFrame* src = impl_->lastFrame;
  if (src->format == AV_PIX_FMT_D3D11) {
    // Zero-copy path: directly use the GPU frame in shared-device mode.
    if (impl_->useSharedDevice) {
        return impl_->emitFrame(out, nullptr, true, src, true);
    }

    av_frame_unref(impl_->scratch);
    if (av_hwframe_transfer_data(impl_->scratch, src, 0) < 0) {
      // Try one more time with a fresh unref
      av_frame_unref(impl_->scratch);
      if (av_hwframe_transfer_data(impl_->scratch, src, 0) < 0) {
        return false;
      }
    }
    src = impl_->scratch;
    src->pts = impl_->lastFrame->pts;
    src->best_effort_timestamp = impl_->lastFrame->best_effort_timestamp;
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

  // Use standard backward seek to find the nearest previous keyframe.
  // This is generally reliable for standard file formats.
  int seekRes = avformat_seek_file(impl_->fmt, -1, INT64_MIN, targetUs, INT64_MAX, AVSEEK_FLAG_BACKWARD);

  if (seekRes < 0) {
    // Fallback: try legacy seek
    int64_t targetStream = av_rescale_q(targetUs, AVRational{1, AV_TIME_BASE}, impl_->timeBase);
    seekRes = av_seek_frame(impl_->fmt, impl_->streamIndex, targetStream, AVSEEK_FLAG_BACKWARD);
  }

  if (seekRes < 0) return false;

  avformat_flush(impl_->fmt);
  avcodec_flush_buffers(impl_->codec);
  if (impl_->frame) {
    av_frame_unref(impl_->frame);
  }
  if (impl_->lastFrame) {
    av_frame_unref(impl_->lastFrame);
  }
  if (impl_->scratch) {
    av_frame_unref(impl_->scratch);
  }
  impl_->atEnd = false;
  impl_->eof = false;
  impl_->consecutiveTransferErrors = 0;
  impl_->seekEpoch++;
  // Reset frame skipping logic - we trust the seek found a keyframe
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

bool probeVideoMetadata(const std::filesystem::path& path, VideoMetadata* out,
                        std::string* error) {
  if (!out) {
    setError(error, "Invalid output for video metadata.");
    return false;
  }
  *out = VideoMetadata{};

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
  if (streamIndex < 0) {
    avformat_close_input(&fmt);
    setError(error, "No video stream found.");
    return false;
  }

  AVStream* stream = fmt->streams[streamIndex];
  const AVCodecParameters* params = stream ? stream->codecpar : nullptr;
  const int rotationQuarterTurns = streamRotationQuarterTurns(stream);
  if (params) {
    applyRotationToDimensions(params->width, params->height,
                              rotationQuarterTurns, &out->width,
                              &out->height);
    out->bitRate = params->bit_rate;
    if (out->bitRate <= 0) out->bitRate = fmt->bit_rate;

    // Check for HDR indicators (PQ or HLG transfer functions)
    if (params->color_trc == AVCOL_TRC_SMPTE2084 ||
        params->color_trc == AVCOL_TRC_ARIB_STD_B67) {
      out->isHDR = true;
    }
  }

  if (codec) {
    out->codecName = codec->name ? codec->name : "";
  } else if (params) {
    const AVCodec* lookup = avcodec_find_decoder(params->codec_id);
    if (lookup && lookup->name) out->codecName = lookup->name;
  }

  int64_t duration100ns = 0;
  if (stream && stream->duration > 0) {
    int64_t us =
        av_rescale_q(stream->duration, stream->time_base, AVRational{1, 1000000});
    duration100ns = us * 10;
  } else if (fmt->duration > 0) {
    duration100ns = fmt->duration * 10;
  }
  out->duration100ns = duration100ns;

  avformat_close_input(&fmt);
  return true;
}
