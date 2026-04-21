#include "player.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <objbase.h>
#include <roapi.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libavutil/display.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "audioplayback.h"
#include "clock.h"
#include "gpu_shared.h"
#include "playback_clock.h"
#include "playback_serial_control.h"
#include "playback_state_policy.h"
#include "playback_sync.h"
#include "queues.h"
#include "runtime_helpers.h"
#include "timing_log.h"
#include "ui_helpers.h"

namespace {
constexpr int64_t kProbeSize = 64 * 1024;
constexpr int64_t kAnalyzeDurationFastUs = 500000;
constexpr int64_t kAnalyzeDurationFallbackUs = 5000000;
constexpr size_t kIoCacheSize = 32 * 1024 * 1024;
constexpr size_t kIoCacheLowWater = 8 * 1024 * 1024;
constexpr size_t kIoCacheHighWater = 24 * 1024 * 1024;
constexpr int kIoAvioBufferSize = 64 * 1024;
constexpr size_t kVideoPrefillFrames = 1;

int64_t frameDurationTicks(const AVFrame* frame) {
  if (!frame) return 0;
#if LIBAVUTIL_VERSION_MAJOR >= 59
  return frame->duration;
#else
  if (frame->duration > 0) return frame->duration;
  return frame->pkt_duration;
#endif
}

const uint8_t* streamSideData(AVStream* stream, AVPacketSideDataType type,
                              size_t* size) {
  if (size) *size = 0;
  if (!stream || !stream->codecpar || !stream->codecpar->coded_side_data) {
    return nullptr;
  }
  const AVPacketSideData* sideData = av_packet_side_data_get(
      stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data,
      type);
  if (!sideData) return nullptr;
  if (size) *size = sideData->size;
  return sideData->data;
}

enum class PlayerEventType {
  SeekRequest,
  PauseRequest,
  ResizeRequest,
  CycleAudioTrack,
  CloseRequest,
  SeekApplied,
  FirstFramePresented,
};

struct PlayerEvent {
  PlayerEventType type = PlayerEventType::SeekRequest;
  int64_t arg1 = 0;
  int64_t arg2 = 0;
  int serial = 0;
};

std::string ffmpegError(int err) {
  char buf[256];
  buf[0] = '\0';
  av_strerror(err, buf, sizeof(buf));
  return std::string(buf);
}

int64_t ptsToUs(int64_t pts, AVRational tb) {
  if (pts == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
  return av_rescale_q(pts, tb, AVRational{1, 1000000});
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
      streamSideData(stream, AV_PKT_DATA_DISPLAYMATRIX, &displayMatrixSize);
  if (displayMatrix &&
      displayMatrixSize >= sizeof(int32_t) * 9u) {
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

// This player implements the following synchronization strategies:
//
// 1. AUDIO STARVATION DETECTION: If the audio buffer runs out of data,
//    pause video playback (sleep) instead of dropping frames. This prevents
//    the deadlock where video keeps advancing but audio can't fill.
//
// 2. SYNC MODES (3-state):
//    - Sync Mode (±100ms): Normal playback with small delay corrections
//    - Catch-up Mode (<-100ms): Drop frames to catch up to audio master
//    - Audio-ahead Mode (>+100ms): Slow video to let audio catch up
//
// 3. SEEK→PRIMING TRANSITION: After a seek, return to Priming state to
//    re-prebuffer both streams. This eliminates post-seek desync.
//
// 4. GRADUAL CLOCK DISCIPLINE: Instead of instant clock resets, gradually
//    adjust playback speed (0.95x-1.01x) to let drifting clocks converge.
//
// 5. AUDIO BUFFER LEVEL FEEDBACK: Monitor the audio buffer level in the
//    video loop. If it's <100ms, slow video playback (0.99x). If it
//    recovers >200ms, gradually return to 1.0x speed.
//
// These 5 strategies eliminate the "frame drop + freeze" cycle by making
// the playback control reactive (audio starving? pause), not speculative.

const char* playerStateName(PlayerState state) {
  switch (state) {
    case PlayerState::Idle:
      return "Idle";
    case PlayerState::Opening:
      return "Opening";
    case PlayerState::Prefill:
      return "Prefill";
    case PlayerState::Priming:
      return "Priming";
    case PlayerState::Playing:
      return "Playing";
    case PlayerState::Paused:
      return "Paused";
    case PlayerState::Seeking:
      return "Seeking";
    case PlayerState::Draining:
      return "Draining";
    case PlayerState::Ended:
      return "Ended";
    case PlayerState::Error:
      return "Error";
    case PlayerState::Closing:
      return "Closing";
  }
  return "Unknown";
}

const char* clockSourceName(PlayerClockSource source) {
  switch (source) {
    case PlayerClockSource::None:
      return "none";
    case PlayerClockSource::Audio:
      return "audio";
    case PlayerClockSource::Video:
      return "video";
  }
  return "none";
}

static enum AVPixelFormat get_hw_format(AVCodecContext* ctx,
                                        const enum AVPixelFormat* pix_fmts) {
  const enum AVPixelFormat* p;
  for (p = pix_fmts; *p != -1; p++) {
    if (*p == AV_PIX_FMT_D3D11) return *p;
  }
  // No CPU fallback: we require D3D11VA frames.
  av_log(ctx, AV_LOG_ERROR,
         "D3D11VA required but decoder offered no AV_PIX_FMT_D3D11.\n");
  return AV_PIX_FMT_NONE;
}

int64_t rescaleToFrames(int64_t value, AVRational src, uint32_t sampleRate) {
  if (value <= 0 || sampleRate == 0) return 0;
  AVRational dst{1, static_cast<int>(sampleRate)};
  return av_rescale_q(value, src, dst);
}

// C-style callbacks for FFmpeg D3D11 locking.
static void d3d11_lock(void* ctx) {
  if (ctx) reinterpret_cast<std::recursive_mutex*>(ctx)->lock();
}

static void d3d11_unlock(void* ctx) {
  if (ctx) reinterpret_cast<std::recursive_mutex*>(ctx)->unlock();
}

struct IoCacheStats {
  size_t size = 0;
  size_t capacity = 0;
  size_t lowWater = 0;
  size_t highWater = 0;
  bool eof = false;
  bool seekPending = false;
  int64_t filePos = 0;
  int64_t fileSize = -1;
};

struct IoCache {
  std::FILE* file = nullptr;
  std::thread thread;
  std::vector<uint8_t> buffer;
  size_t capacity = 0;
  size_t lowWater = 0;
  size_t highWater = 0;
  size_t readPos = 0;
  size_t writePos = 0;
  size_t size = 0;
  int64_t fileSize = -1;
  int64_t filePos = 0;
  bool eof = false;
  bool aborted = false;
  bool seekPending = false;
  int64_t seekOffset = 0;
  int seekWhence = SEEK_SET;
  bool seekDone = false;
  mutable std::mutex mutex;
  std::condition_variable cv;
  std::atomic<bool>* running = nullptr;
  std::atomic<bool>* commandPending = nullptr;

  bool open(const std::filesystem::path& path, std::string* error) {
    file = openFileUtf8(path, "rb");
    if (!file) {
      if (error) *error = "Failed to open media file for caching.";
      return false;
    }
    std::error_code ec;
    fileSize = static_cast<int64_t>(std::filesystem::file_size(path, ec));
    if (ec) {
      fileSize = -1;
    }
    capacity = kIoCacheSize;
    lowWater = kIoCacheLowWater;
    highWater = kIoCacheHighWater;
    buffer.resize(capacity);
    return true;
  }

  void start() {
    aborted = false;
    eof = false;
    seekPending = false;
    seekDone = false;
    readPos = 0;
    writePos = 0;
    size = 0;
    filePos = 0;
    thread = std::thread([this]() { threadMain(); });
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      aborted = true;
      cv.notify_all();
    }
    if (thread.joinable()) {
      thread.join();
    }
    if (file) {
      std::fclose(file);
      file = nullptr;
    }
  }

  IoCacheStats stats() const {
    std::lock_guard<std::mutex> lock(mutex);
    IoCacheStats out;
    out.size = size;
    out.capacity = capacity;
    out.lowWater = lowWater;
    out.highWater = highWater;
    out.eof = eof;
    out.seekPending = seekPending;
    out.filePos = filePos;
    out.fileSize = fileSize;
    return out;
  }

  int read(uint8_t* out, int outSize) {
    if (!out || outSize <= 0) return AVERROR_EOF;
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&]() {
      bool run = running ? running->load() : true;
      return aborted || !run || size > 0 || eof ||
             (commandPending && commandPending->load());
    });

    if (aborted) return AVERROR_EXIT;
    if (commandPending && commandPending->load()) {
      return AVERROR_EXIT;
    }
    if (size == 0 && eof) return AVERROR_EOF;
    if (size == 0) return AVERROR(EAGAIN);

    size_t toRead = std::min<size_t>(size, static_cast<size_t>(outSize));
    size_t first = std::min(toRead, capacity - readPos);
    std::memcpy(out, buffer.data() + readPos, first);
    if (toRead > first) {
      std::memcpy(out + first, buffer.data(), toRead - first);
    }
    readPos = (readPos + toRead) % capacity;
    size -= toRead;
    cv.notify_all();
    return static_cast<int>(toRead);
  }

  int64_t seek(int64_t offset, int whence) {
    std::unique_lock<std::mutex> lock(mutex);
    if (aborted) return AVERROR_EXIT;
    const int baseWhence = whence & ~AVSEEK_FORCE;
    if (baseWhence == AVSEEK_SIZE) {
      return fileSize;
    }
    seekPending = true;
    seekDone = false;
    seekOffset = offset;
    seekWhence = baseWhence;
    eof = false;
    cv.notify_all();

    cv.wait(lock, [&]() {
      bool run = running ? running->load() : true;
      return aborted || !run || seekDone;
    });
    if (aborted) return AVERROR_EXIT;
    return filePos;
  }

  void threadMain() {
    bool filling = true;
    while (true) {
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&]() {
          bool run = running ? running->load() : true;
          return aborted || !run || seekPending ||
                 (filling && size < highWater) || (!filling && size < lowWater);
        });
        bool run = running ? running->load() : true;
        if (aborted || !run) {
          break;
        }
        if (seekPending) {
          int whence = seekWhence;
          if (whence == AVSEEK_SIZE) {
            seekPending = false;
            seekDone = true;
            cv.notify_all();
            continue;
          }
          int64_t base = 0;
          if (whence == SEEK_SET) {
            base = 0;
          } else if (whence == SEEK_CUR) {
            base = filePos;
          } else if (whence == SEEK_END) {
            base = fileSize;
          }
          int64_t newPos = base + seekOffset;
          if (newPos < 0) newPos = 0;
          if (fileSize > 0 && newPos > fileSize) newPos = fileSize;

          bool seekOk = true;
#if defined(_WIN32)
          seekOk =
              (_fseeki64(file, static_cast<__int64>(newPos), SEEK_SET) == 0);
#else
          seekOk = (std::fseek(file, static_cast<long>(newPos), SEEK_SET) == 0);
#endif

          filePos = seekOk ? newPos : static_cast<int64_t>(AVERROR(EIO));
          readPos = 0;
          writePos = 0;
          size = 0;
          eof = !seekOk;
          seekPending = false;
          seekDone = true;
          filling = true;
          cv.notify_all();
          continue;
        }
      }

      if (commandPending && commandPending->load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      size_t space = 0;
      size_t writePosLocal = 0;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (size >= capacity) {
          cv.notify_all();
          continue;
        }
        space = capacity - size;
        writePosLocal = writePos;
      }

      size_t first = std::min(space, capacity - writePosLocal);
      size_t toRead = std::min(first, static_cast<size_t>(kIoAvioBufferSize));
      size_t readCount = std::fread(buffer.data() + writePosLocal, 1, toRead,
                                    file);

      {
        std::lock_guard<std::mutex> lock(mutex);
        if (readCount == 0) {
          bool atEof = std::feof(file) != 0;
          if (!atEof && fileSize >= 0 && filePos >= fileSize) {
            atEof = true;
          }
          if (atEof) {
            eof = true;
            filling = false;
            cv.notify_all();
            continue;
          }
          if (std::ferror(file)) {
            std::clearerr(file);
          }
          cv.notify_all();
          continue;
        }
        writePos = (writePos + readCount) % capacity;
        size += readCount;
        filePos += static_cast<int64_t>(readCount);
        if (size >= highWater) {
          filling = false;
        }
        cv.notify_all();
      }
    }
  }
};

int ioReadPacket(void* opaque, uint8_t* buf, int buf_size) {
  auto* cache = reinterpret_cast<IoCache*>(opaque);
  if (!cache) return AVERROR_EXIT;
  return cache->read(buf, buf_size);
}

int64_t ioSeek(void* opaque, int64_t offset, int whence) {
  auto* cache = reinterpret_cast<IoCache*>(opaque);
  if (!cache) return AVERROR_EXIT;
  return cache->seek(offset, whence);
}

std::string streamMetadataValue(const AVStream* stream, const char* key) {
  if (!stream || !stream->metadata || !key) return {};
  const AVDictionaryEntry* entry = av_dict_get(stream->metadata, key, nullptr, 0);
  if (!entry || !entry->value || entry->value[0] == '\0') return {};
  return std::string(entry->value);
}

std::string formatAudioTrackLabel(const AVStream* stream, size_t ordinal) {
  std::string language = streamMetadataValue(stream, "language");
  std::string title = streamMetadataValue(stream, "title");
  if (!title.empty() && !language.empty()) {
    return language + " - " + title;
  }
  if (!language.empty()) return language;
  if (!title.empty()) return title;
  return "Track " + std::to_string(ordinal + 1);
}

struct DemuxContext {
  AVFormatContext* fmt = nullptr;
  AVIOContext* avio = nullptr;
  std::unique_ptr<IoCache> io;
  int videoStreamIndex = -1;
  int audioStreamIndex = -1;
  std::vector<int> audioStreamIndices;
  std::vector<std::string> audioTrackLabels;
  int activeAudioTrack = -1;
  AVRational videoTimeBase{0, 1};
  AVRational audioTimeBase{0, 1};
  int64_t formatStartUs = 0;
  int64_t videoStartUs = 0;
  int64_t audioStartUs = 0;
  int64_t durationUs = 0;
};

struct DemuxInterrupt {
  std::atomic<bool>* running = nullptr;
  std::atomic<bool>* commandPending = nullptr;
  std::atomic<int>* currentSerial = nullptr;
  int lastSerial = 0;
};

int demuxInterruptCallback(void* opaque) {
  auto* interrupt = reinterpret_cast<DemuxInterrupt*>(opaque);
  if (!interrupt) return 0;
  if (interrupt->running && !interrupt->running->load()) return 1;
  if (interrupt->commandPending && interrupt->commandPending->load()) return 1;
  if (interrupt->currentSerial && interrupt->currentSerial->load() != interrupt->lastSerial) return 1;
  return 0;
}

struct VideoDecodeContext {
  AVCodecContext* codec = nullptr;
  AVBufferRef* hwDeviceCtx = nullptr;
  AVFrame* frame = nullptr;
  AVFrame* scratch = nullptr;
  SwsContext* sws = nullptr;
  AVRational timeBase{0, 1};
  int width = 0;
  int height = 0;
  int targetW = 0;
  int targetH = 0;
  int rotationQuarterTurns = 0;
  int64_t formatStartUs = 0;
  YuvMatrix yuvMatrix = YuvMatrix::Bt709;
  YuvTransfer yuvTransfer = YuvTransfer::Sdr;
  bool fullRange = true;
  bool useSharedDevice = false;
};

struct AudioDecodeContext {
  AVCodecContext* codec = nullptr;
  AVFrame* frame = nullptr;
  SwrContext* swr = nullptr;
  AVRational timeBase{0, 1};
  int64_t formatStartUs = 0;
  uint32_t outRate = 48000;
  uint32_t outChannels = 2;
  uint64_t pendingSkipFrames = 0;
  int64_t writePosFrames = 0;
  int64_t nextPtsUs = 0;
  bool nextPtsValid = false;
  std::vector<float> convertBuffer;
};

bool openInputWithProbe(const std::filesystem::path& path,
                        int64_t analyzeDurationUs,
                        AVFormatContext** outFmt,
                        AVIOContext** outPb,
                        IoCache* ioCache,
                        std::string* error) {
  AVDictionary* options = nullptr;
  av_dict_set_int(&options, "probesize", kProbeSize, 0);
  av_dict_set_int(&options, "analyzeduration", analyzeDurationUs, 0);

  if (outPb) {
    *outPb = nullptr;
  }

  AVFormatContext* fmt = nullptr;
  AVIOContext* avio = nullptr;
  if (ioCache) {
    if (!ioCache->open(path, error)) {
      av_dict_free(&options);
      return false;
    }
    ioCache->start();
    fmt = avformat_alloc_context();
    if (!fmt) {
      ioCache->stop();
      av_dict_free(&options);
      if (error) *error = "Failed to allocate format context.";
      return false;
    }
    unsigned char* buffer =
        reinterpret_cast<unsigned char*>(av_malloc(kIoAvioBufferSize));
    if (!buffer) {
      avformat_free_context(fmt);
      ioCache->stop();
      av_dict_free(&options);
      if (error) *error = "Failed to allocate IO buffer.";
      return false;
    }
    avio = avio_alloc_context(buffer, kIoAvioBufferSize, 0, ioCache,
                              &ioReadPacket, nullptr, &ioSeek);
    if (!avio) {
      av_free(buffer);
      avformat_free_context(fmt);
      ioCache->stop();
      av_dict_free(&options);
      if (error) *error = "Failed to allocate AVIO context.";
      return false;
    }
    avio->seekable = AVIO_SEEKABLE_NORMAL;
    fmt->pb = avio;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
  }

  const std::string pathUtf8 = ioCache ? std::string() : toUtf8String(path);
  int openRes =
      avformat_open_input(&fmt, ioCache ? nullptr : pathUtf8.c_str(), nullptr,
                          &options);
  av_dict_free(&options);
  if (openRes < 0) {
    std::string msg = "Failed to open media: " + ffmpegError(openRes);
    if (error) *error = msg;
    if (avio) {
      avio_context_free(&avio);
    }
    if (fmt) {
      avformat_free_context(fmt);
    }
    if (ioCache) {
      ioCache->stop();
    }
    return false;
  }
  if (outPb && avio) {
    *outPb = avio;
  }
  if (outFmt) {
    *outFmt = fmt;
    (*outFmt)->max_analyze_duration = analyzeDurationUs;
    (*outFmt)->probesize = kProbeSize;
  }
  return true;
}

bool initDemuxer(const std::filesystem::path& path, DemuxContext* out,
                 std::string* error, DemuxInterrupt* interrupt) {
  if (!out) return false;
  auto ioCache = std::make_unique<IoCache>();
  if (interrupt) {
    ioCache->running = interrupt->running;
    ioCache->commandPending = interrupt->commandPending;
  }
  AVFormatContext* fmt = nullptr;
  AVIOContext* avio = nullptr;
  if (!openInputWithProbe(path, kAnalyzeDurationFastUs, &fmt, &avio,
                          ioCache.get(), error)) {
    return false;
  }
  fmt->flags |= AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_GENPTS | AVFMT_FLAG_DISCARD_CORRUPT;
  int infoErr = avformat_find_stream_info(fmt, nullptr);
  if (infoErr < 0) {
    avformat_close_input(&fmt);
    if (avio) {
      avio_context_free(&avio);
      avio = nullptr;
    }
    ioCache->stop();
    if (!openInputWithProbe(path, kAnalyzeDurationFallbackUs, &fmt, &avio,
                            ioCache.get(), error)) {
      return false;
    }
    fmt->flags |= AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_GENPTS | AVFMT_FLAG_DISCARD_CORRUPT;
    infoErr = avformat_find_stream_info(fmt, nullptr);
    if (infoErr < 0) {
      std::string msg = "Failed to read stream info: " + ffmpegError(infoErr);
      avformat_close_input(&fmt);
      if (avio) {
        avio_context_free(&avio);
      }
      ioCache->stop();
      if (error) *error = msg;
      return false;
    }
  }

  out->fmt = fmt;
  out->avio = avio;
  out->io = std::move(ioCache);
  if (interrupt) {
    out->fmt->interrupt_callback.callback = demuxInterruptCallback;
    out->fmt->interrupt_callback.opaque = interrupt;
  }
  out->formatStartUs = (fmt->start_time != AV_NOPTS_VALUE) ? fmt->start_time : 0;
  out->durationUs = (fmt->duration > 0) ? fmt->duration : 0;

  const AVCodec* vcodec = nullptr;
  out->videoStreamIndex =
      av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &vcodec, 0);
  if (out->videoStreamIndex >= 0) {
    AVStream* stream = fmt->streams[out->videoStreamIndex];
    out->videoTimeBase = stream->time_base;
    if (stream->start_time != AV_NOPTS_VALUE) {
      out->videoStartUs = av_rescale_q(stream->start_time, stream->time_base,
                                       AVRational{1, AV_TIME_BASE});
    } else {
      out->videoStartUs = out->formatStartUs;
    }
  }
    out->audioStreamIndices.clear();
    out->audioTrackLabels.clear();
    out->activeAudioTrack = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
      AVStream* stream = fmt->streams[i];
      if (!stream || !stream->codecpar) continue;
      if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
      out->audioStreamIndices.push_back(static_cast<int>(i));
      out->audioTrackLabels.push_back(
          formatAudioTrackLabel(stream, out->audioTrackLabels.size()));
    }

    const AVCodec* acodec = nullptr;
    out->audioStreamIndex =
        av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &acodec, 0);
    if (out->audioStreamIndex < 0 && !out->audioStreamIndices.empty()) {
      out->audioStreamIndex = out->audioStreamIndices.front();
    }
    if (out->audioStreamIndex >= 0) {
      auto it = std::find(out->audioStreamIndices.begin(),
                          out->audioStreamIndices.end(), out->audioStreamIndex);
      if (it != out->audioStreamIndices.end()) {
        out->activeAudioTrack =
            static_cast<int>(std::distance(out->audioStreamIndices.begin(), it));
      }
      AVStream* stream = fmt->streams[out->audioStreamIndex];
      out->audioTimeBase = stream->time_base;
      if (stream->start_time != AV_NOPTS_VALUE) {
      out->audioStartUs = av_rescale_q(stream->start_time, stream->time_base,
                                       AVRational{1, AV_TIME_BASE});
    } else {
        out->audioStartUs = out->formatStartUs;
      }
    }
    if (out->activeAudioTrack < 0 && !out->audioStreamIndices.empty()) {
      out->activeAudioTrack = 0;
    }

  if (out->videoStreamIndex < 0 && out->audioStreamIndex < 0) {
    avformat_close_input(&fmt);
    if (error) *error = "No playable streams found.";
    return false;
  }
  return true;
}

bool initVideoDecoder(const DemuxContext& demux, bool preferHardware,
                      VideoDecodeContext* out, std::string* error) {
  if (!out || demux.videoStreamIndex < 0) return false;
  AVStream* stream = demux.fmt->streams[demux.videoStreamIndex];
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) {
    if (error) *error = "No video decoder found.";
    return false;
  }
  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if (!ctx) {
    if (error) *error = "Failed to allocate video decoder.";
    return false;
  }
  if (avcodec_parameters_to_context(ctx, stream->codecpar) < 0) {
    avcodec_free_context(&ctx);
    if (error) *error = "Failed to configure video decoder.";
    return false;
  }

  ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
  ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
  ctx->err_recognition = AV_EF_IGNORE_ERR;
  if (ctx->thread_count < 0) {
    ctx->thread_count = 0;
  }

  AVBufferRef* hwDeviceCtx = nullptr;
  bool usingSharedDevice = false;
  if (preferHardware) {
    ID3D11Device* sharedDevice = getSharedGpuDevice();
    if (!sharedDevice) {
      avcodec_free_context(&ctx);
      if (error) {
        *error =
            "Video CPU fallback disabled: shared GPU device is unavailable.";
      }
      return false;
    }

    AVBufferRef* sharedCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!sharedCtx) {
      avcodec_free_context(&ctx);
      if (error) {
        *error =
            "Video CPU fallback disabled: failed to allocate D3D11VA device "
            "context.";
      }
      return false;
    }

    AVHWDeviceContext* deviceCtx =
        reinterpret_cast<AVHWDeviceContext*>(sharedCtx->data);
    AVD3D11VADeviceContext* d3d11Ctx =
        reinterpret_cast<AVD3D11VADeviceContext*>(deviceCtx->hwctx);
    sharedDevice->AddRef();
    d3d11Ctx->device = sharedDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediateContext;
    sharedDevice->GetImmediateContext(&immediateContext);
    immediateContext->AddRef();
    d3d11Ctx->device_context = immediateContext.Get();
    d3d11Ctx->lock = d3d11_lock;
    d3d11Ctx->unlock = d3d11_unlock;
    d3d11Ctx->lock_ctx = &getSharedGpuMutex();

    int hwInit = av_hwdevice_ctx_init(sharedCtx);
    if (hwInit < 0) {
      av_buffer_unref(&sharedCtx);
      avcodec_free_context(&ctx);
      if (error) {
        *error = "Video CPU fallback disabled: failed to init shared D3D11VA "
                 "device context (" +
                 ffmpegError(hwInit) + ").";
      }
      return false;
    }

    hwDeviceCtx = sharedCtx;
    usingSharedDevice = true;

    ctx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
    ctx->get_format = get_hw_format;
    ctx->extra_hw_frames = 32;
    if (!configureD3d11Frames(ctx, ctx->hw_device_ctx, true)) {
      if (hwDeviceCtx) av_buffer_unref(&hwDeviceCtx);
      avcodec_free_context(&ctx);
      if (error) {
        *error =
            "Video CPU fallback disabled: failed to configure D3D11VA frames.";
      }
      return false;
    }
  }

  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    if (hwDeviceCtx) av_buffer_unref(&hwDeviceCtx);
    avcodec_free_context(&ctx);
    if (error) *error = "Failed to open video decoder.";
    return false;
  }

  if (preferHardware) {
    if (!hwDeviceCtx || !usingSharedDevice || ctx->get_format != get_hw_format) {
      if (hwDeviceCtx) av_buffer_unref(&hwDeviceCtx);
      avcodec_free_context(&ctx);
      if (error) {
        *error = "Video CPU fallback disabled: D3D11VA hardware decode is not "
                 "active.";
      }
      return false;
    }
  }

  AVFrame* frame = av_frame_alloc();
  AVFrame* scratch = av_frame_alloc();
  if (!frame || !scratch) {
    av_frame_free(&frame);
    av_frame_free(&scratch);
    if (hwDeviceCtx) av_buffer_unref(&hwDeviceCtx);
    avcodec_free_context(&ctx);
    if (error) *error = "Failed to allocate video buffers.";
    return false;
  }

  out->codec = ctx;
  out->hwDeviceCtx = hwDeviceCtx;
  out->frame = frame;
  out->scratch = scratch;
  out->timeBase = stream->time_base;
  out->rotationQuarterTurns = streamRotationQuarterTurns(stream);
  applyRotationToDimensions(ctx->width, ctx->height,
                            out->rotationQuarterTurns, &out->width,
                            &out->height);
  out->targetW = out->width;
  out->targetH = out->height;
  if (!usingSharedDevice) {
    clampTargetSize(out->targetW, out->targetH);
  } else if (out->rotationQuarterTurns != 0) {
    // Shared-device playback rotates in GPU; keep fallback targets even.
    normalizeEvenTargetSize(out->targetW, out->targetH);
  }
  out->fullRange = mapFullRange(ctx->color_range);
  out->yuvMatrix = mapColorMatrix(ctx->colorspace);
  out->yuvTransfer = mapColorTransfer(ctx->color_trc);
  out->formatStartUs = demux.formatStartUs;
  out->useSharedDevice = usingSharedDevice;
  return true;
}

bool initAudioDecoder(const DemuxContext& demux, int streamIndex, uint32_t outRate,
                       uint32_t outChannels, AudioDecodeContext* out,
                       std::string* error) {
    if (!out || !demux.fmt || streamIndex < 0 ||
        streamIndex >= static_cast<int>(demux.fmt->nb_streams)) {
      return false;
    }
    AVStream* stream = demux.fmt->streams[streamIndex];
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) {
    if (error) *error = "No audio decoder found.";
    return false;
  }
  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if (!ctx) {
    if (error) *error = "Failed to allocate audio decoder.";
    return false;
  }
  if (avcodec_parameters_to_context(ctx, stream->codecpar) < 0) {
    avcodec_free_context(&ctx);
    if (error) *error = "Failed to configure audio decoder.";
    return false;
  }

  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    avcodec_free_context(&ctx);
    if (error) *error = "Failed to open audio decoder.";
    return false;
  }

  SwrContext* swr = nullptr;
  AVChannelLayout outLayout;
  av_channel_layout_default(&outLayout, static_cast<int>(outChannels));
  AVChannelLayout inLayout;
  if (av_channel_layout_copy(&inLayout, &ctx->ch_layout) < 0) {
    av_channel_layout_uninit(&outLayout);
    avcodec_free_context(&ctx);
    if (error) *error = "Failed to configure audio channel layout.";
    return false;
  }

  if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT,
                          static_cast<int>(outRate), &inLayout,
                          ctx->sample_fmt, ctx->sample_rate, 0, nullptr) < 0) {
    av_channel_layout_uninit(&inLayout);
    av_channel_layout_uninit(&outLayout);
    avcodec_free_context(&ctx);
    if (error) *error = "Failed to configure audio resampler.";
    return false;
  }
  if (swr_init(swr) < 0) {
    av_channel_layout_uninit(&inLayout);
    av_channel_layout_uninit(&outLayout);
    swr_free(&swr);
    avcodec_free_context(&ctx);
    if (error) *error = "Failed to initialize audio resampler.";
    return false;
  }
  av_channel_layout_uninit(&inLayout);
  av_channel_layout_uninit(&outLayout);

  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    swr_free(&swr);
    avcodec_free_context(&ctx);
    if (error) *error = "Failed to allocate audio frame.";
    return false;
  }

  out->codec = ctx;
  out->frame = frame;
  out->swr = swr;
  out->timeBase = stream->time_base;
  out->formatStartUs = demux.formatStartUs;
  out->outRate = outRate;
  out->outChannels = outChannels;
  if (stream->codecpar && stream->codecpar->initial_padding > 0) {
    uint32_t inRate =
        ctx->sample_rate > 0 ? static_cast<uint32_t>(ctx->sample_rate) : outRate;
    out->pendingSkipFrames = static_cast<uint64_t>(rescaleToFrames(
        static_cast<int64_t>(stream->codecpar->initial_padding),
        AVRational{1, static_cast<int>(inRate)}, outRate));
  }
  out->writePosFrames = 0;
  out->nextPtsUs = 0;
  out->nextPtsValid = false;
  return true;
}

void destroyVideoDecoder(VideoDecodeContext* ctx) {
  if (!ctx) return;
  if (ctx->sws) {
    sws_freeContext(ctx->sws);
    ctx->sws = nullptr;
  }
  if (ctx->hwDeviceCtx) {
    av_buffer_unref(&ctx->hwDeviceCtx);
  }
  av_frame_free(&ctx->frame);
  av_frame_free(&ctx->scratch);
  if (ctx->codec) {
    avcodec_free_context(&ctx->codec);
  }
}

void destroyAudioDecoder(AudioDecodeContext* ctx) {
  if (!ctx) return;
  if (ctx->swr) {
    swr_free(&ctx->swr);
  }
  av_frame_free(&ctx->frame);
  if (ctx->codec) {
    avcodec_free_context(&ctx->codec);
  }
}

int64_t frameTimestamp100ns(const VideoDecodeContext& ctx,
                            const AVFrame* src) {
  if (!src) return 0;
  int64_t bestPts = src->best_effort_timestamp;
  if (bestPts == AV_NOPTS_VALUE) {
    bestPts = src->pts;
  }
  if (bestPts == AV_NOPTS_VALUE) return 0;
  int64_t absUs =
      av_rescale_q(bestPts, ctx.timeBase, AVRational{1, AV_TIME_BASE});
  int64_t relUs = absUs - ctx.formatStartUs;
  if (relUs < 0) relUs = 0;
  return relUs * 10;
}

int64_t frameDuration100ns(const VideoDecodeContext& ctx,
                           const AVFrame* src) {
  if (!src) return 0;
  int64_t duration = frameDurationTicks(src);
  if (duration <= 0) return 0;
  int64_t durUs =
      av_rescale_q(duration, ctx.timeBase, AVRational{1, AV_TIME_BASE});
  if (durUs <= 0) return 0;
  return durUs * 10;
}

bool emitVideoFrame(VideoDecodeContext* ctx, VideoFrame& out,
                    VideoReadInfo* info, AVFrame* src, bool decodePixels,
                    bool keepOnGpu) {
  if (!ctx || !src) return false;

  auto frameRef = [](const AVFrame* frame) -> std::shared_ptr<AVFrame> {
    if (!frame) return {};
    AVFrame* copy = av_frame_alloc();
    if (!copy) return {};
    if (av_frame_ref(copy, frame) < 0) {
      av_frame_free(&copy);
      return {};
    }
    return std::shared_ptr<AVFrame>(copy,
                                    [](AVFrame* f) { av_frame_free(&f); });
  };

  int64_t ts100ns = frameTimestamp100ns(*ctx, src);
  int64_t duration100ns = frameDuration100ns(*ctx, src);
  AVColorSpace frameSpace = src->colorspace;
  if (frameSpace == AVCOL_SPC_UNSPECIFIED ||
      frameSpace == AVCOL_SPC_RESERVED) {
    frameSpace = ctx->codec ? ctx->codec->colorspace : frameSpace;
  }
  AVColorPrimaries framePrimaries = src->color_primaries;
  if (framePrimaries == AVCOL_PRI_UNSPECIFIED ||
      framePrimaries == AVCOL_PRI_RESERVED) {
    framePrimaries = ctx->codec ? ctx->codec->color_primaries
                                : framePrimaries;
  }
  YuvMatrix frameMatrix = ctx->yuvMatrix;
  if (frameSpace != AVCOL_SPC_UNSPECIFIED &&
      frameSpace != AVCOL_SPC_RESERVED) {
    frameMatrix = mapColorMatrix(frameSpace);
  } else if (framePrimaries == AVCOL_PRI_BT2020) {
    frameMatrix = YuvMatrix::Bt2020;
  }
  AVColorTransferCharacteristic frameTrc = src->color_trc;
  if (frameTrc == AVCOL_TRC_UNSPECIFIED || frameTrc == AVCOL_TRC_RESERVED) {
    frameTrc = ctx->codec ? ctx->codec->color_trc : frameTrc;
  }
  YuvTransfer frameTransfer = ctx->yuvTransfer;
  if (frameTrc != AVCOL_TRC_UNSPECIFIED && frameTrc != AVCOL_TRC_RESERVED) {
    frameTransfer = mapColorTransfer(frameTrc);
  } else if (framePrimaries == AVCOL_PRI_BT2020 ||
             frameMatrix == YuvMatrix::Bt2020) {
    frameTransfer = YuvTransfer::Pq;
  }

  out.hwFrameRef.reset();
  out.hwTexture.Reset();
  out.hwTextureArrayIndex = 0;

  const int rotationTurns =
      normalizeQuarterTurns(ctx->rotationQuarterTurns);
  bool canKeepOnGpu = keepOnGpu && src->format == AV_PIX_FMT_D3D11;
  std::shared_ptr<AVFrame> gpuFrameRef;
  if (canKeepOnGpu) {
    gpuFrameRef = frameRef(src);
    if (!gpuFrameRef) {
      canKeepOnGpu = false;
    }
  }

  int outW = canKeepOnGpu ? src->width : ctx->targetW;
  int outH = canKeepOnGpu ? src->height : ctx->targetH;
  out.width = outW;
  out.height = outH;
  out.timestamp100ns = ts100ns;
  out.fullRange = ctx->fullRange;
  out.yuvMatrix = frameMatrix;
  out.yuvTransfer = frameTransfer;
  out.rotationQuarterTurns = 0;

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

  if (canKeepOnGpu) {
    auto* texture = reinterpret_cast<ID3D11Texture2D*>(src->data[0]);
    if (!texture) return false;
    intptr_t arrayIndex = reinterpret_cast<intptr_t>(src->data[1]);
    out.format = VideoPixelFormat::HWTexture;
    out.hwTexture = texture;
    out.hwTextureArrayIndex = static_cast<int>(arrayIndex);
    out.hwFrameRef = std::move(gpuFrameRef);
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

  AVFrame* cpuSrc = src;
  if (src->format == AV_PIX_FMT_D3D11) {
    av_frame_unref(ctx->scratch);
    if (av_hwframe_transfer_data(ctx->scratch, src, 0) < 0) {
      av_frame_unref(ctx->scratch);
      if (av_hwframe_transfer_data(ctx->scratch, src, 0) < 0) {
        return false;
      }
    }
    ctx->scratch->pts = src->pts;
    ctx->scratch->best_effort_timestamp = src->best_effort_timestamp;
    cpuSrc = ctx->scratch;
  }

  int dstW = ctx->targetW;
  int dstH = ctx->targetH;
  int stride = std::max(2, dstW);
  if (stride & 1) ++stride;
  int planeHeight = dstH;
  size_t required = static_cast<size_t>(stride) *
                    static_cast<size_t>(planeHeight) * 3u / 2u;
  try {
    out.yuv.resize(required);
  } catch (const std::bad_alloc&) {
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
  uint8_t* scaledUV = scaledY +
                      static_cast<size_t>(stride) *
                          static_cast<size_t>(planeHeight);
  if (rotationTurns != 0) {
    size_t scaledRequired = static_cast<size_t>(scaledStride) *
                            static_cast<size_t>(scaledPlaneHeight) * 3u / 2u;
    try {
      scaled.resize(scaledRequired);
    } catch (const std::bad_alloc&) {
      return false;
    }
    scaledY = scaled.data();
    scaledUV = scaledY + static_cast<size_t>(scaledStride) *
                             static_cast<size_t>(scaledPlaneHeight);
  }

  uint8_t* dstData[4] = {scaledY, scaledUV, nullptr, nullptr};
  int dstLinesize[4] = {scaledStride, scaledStride, 0, 0};

  AVPixelFormat srcFmt = static_cast<AVPixelFormat>(cpuSrc->format);
  if (srcFmt == AV_PIX_FMT_NV12 && cpuSrc->width == scaledW &&
      cpuSrc->height == scaledH) {
    for (int y = 0; y < scaledH; ++y) {
      std::memcpy(scaledY + y * scaledStride,
                  cpuSrc->data[0] + y * cpuSrc->linesize[0],
                  static_cast<size_t>(scaledW));
    }
    int uvH = scaledH / 2;
    for (int y = 0; y < uvH; ++y) {
      std::memcpy(scaledUV + y * scaledStride,
                  cpuSrc->data[1] + y * cpuSrc->linesize[1],
                  static_cast<size_t>(scaledW));
    }
  } else {
    int flags = SWS_FAST_BILINEAR;
    if (static_cast<int64_t>(cpuSrc->width) * cpuSrc->height > 2560 * 1440) {
      flags = SWS_POINT;
    }
    ctx->sws = sws_getCachedContext(
        ctx->sws, cpuSrc->width, cpuSrc->height, srcFmt, scaledW, scaledH,
        AV_PIX_FMT_NV12, flags, nullptr, nullptr, nullptr);
    if (!ctx->sws) {
      return false;
    }
    sws_scale(ctx->sws, cpuSrc->data, cpuSrc->linesize, 0, cpuSrc->height,
              dstData, dstLinesize);
  }

  if (rotationTurns != 0) {
    if (!rotateNv12(scaled.data(), scaledStride, scaledPlaneHeight, scaledW,
                    scaledH, rotationTurns, out.yuv.data(), stride,
                    planeHeight, dstW, dstH)) {
      return false;
    }
  }

  if (info) {
    info->timestamp100ns = ts100ns;
    info->duration100ns = duration100ns;
  }
  return true;
}
}  // namespace

struct Player::Impl {
  PlayerConfig config;
  std::atomic<bool> running{false};
  std::atomic<bool> ctrlRunning{false};
  std::atomic<PlayerState> state{PlayerState::Idle};
  std::atomic<bool> pauseRequested{false};
  playback_serial_control::Controller serialControl;
  std::mutex eventMutex;
  std::condition_variable eventCv;
  std::deque<PlayerEvent> events;
  std::thread controlThread;
  std::atomic<bool> initDone{false};
  std::atomic<bool> initOk{false};
  std::mutex initMutex;
  std::string initError;

  std::atomic<bool> audioStartDone{false};
  std::atomic<bool> audioStartOk{false};

  std::atomic<bool> decodeEnded{false};
  std::atomic<bool> demuxEnded{false};
  std::atomic<bool> audioDecodeEnded{false};

  std::atomic<bool> commandPending{false};
  std::atomic<uint64_t> resizeEpoch{0};
  std::atomic<int> resizeTargetW{0};
  std::atomic<int> resizeTargetH{0};

    std::atomic<size_t> maxQueue{3};
    std::atomic<int64_t> fallbackFrameDurationUs{33333};
    std::atomic<int64_t> durationUs{0};
    std::atomic<int> sourceWidth{0};
    std::atomic<int> sourceHeight{0};
    std::mutex audioTrackMutex;
    std::vector<int> audioTrackStreams;
    std::vector<std::string> audioTrackLabels;
    std::atomic<int> activeAudioTrack{-1};
    std::atomic<int> activeAudioStream{-1};

  std::atomic<int64_t> lastPresentedPtsUs{0};
  std::atomic<int64_t> lastPresentedDurationUs{0};
  std::atomic<int> lastPresentedSerial{0};
  std::atomic<int64_t> lastMasterUs{0};
  std::atomic<int> lastMasterSource{0};
  std::atomic<int64_t> lastDiffUs{0};
  std::atomic<int64_t> lastDelayUs{0};
  std::atomic<int64_t> audioBufferedStartPtsUs{0};
  std::atomic<int> audioBufferedStartSerial{0};
  std::atomic<bool> audioBufferedStartValid{false};

  std::atomic<bool> clearFrameRequested{false};
  std::atomic<bool> hasFrame{false};
  std::mutex currentFrameMutex;
  VideoFrame currentFrame;
  std::condition_variable frameCv;
  std::mutex frameCvMutex;
  std::atomic<uint64_t> frameCounter{0};
  std::atomic<uint64_t> lastReadCounter{0};
  HANDLE frameReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  std::mutex lastInfoMutex;
  VideoReadInfo lastInfo{};
  std::atomic<bool> lastInfoValid{false};

  Clock videoClock;

  PacketQueue videoPackets;
  PacketQueue audioPackets;
  FrameQueue videoFrames;
  DemuxContext demux{};
  VideoDecodeContext videoDec{};
  AudioDecodeContext audioDec{};
  DemuxInterrupt demuxInterrupt{};

  std::thread demuxThread;
  std::thread videoDecodeThread;
  std::thread audioDecodeThread;
  std::thread videoOutputThread;

  std::filesystem::path logPath;

  void appendTiming(const std::string& line) {
#if RADIOIFY_ENABLE_TIMING_LOG
    if (logPath.empty()) return;
    std::lock_guard<std::mutex> lock(timingLogMutex());
    static std::ofstream f;
    static std::filesystem::path currentPath;
    if (currentPath != logPath) {
      if (f.is_open()) f.close();
      f.open(logPath, std::ios::app);
      currentPath = logPath;
    }
    if (!f) return;
    f << radioifyLogTimestamp() << " " << line << "\n";
#else
    (void)line;
#endif
  }

  void appendTimingFmt(const char* fmt, ...) {
#if RADIOIFY_ENABLE_TIMING_LOG
    if (logPath.empty()) return;
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    int written = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (written <= 0) return;
    if (written >= static_cast<int>(sizeof(buf))) {
      written = static_cast<int>(sizeof(buf)) - 1;
    }
    std::lock_guard<std::mutex> lock(timingLogMutex());
    static std::ofstream f;
    static std::filesystem::path currentPath;
    if (currentPath != logPath) {
      if (f.is_open()) f.close();
      f.open(logPath, std::ios::app);
      currentPath = logPath;
    }
    if (!f) return;
    f << radioifyLogTimestamp() << " " << std::string(buf, buf + written)
      << "\n";
    f.flush();
#else
    (void)fmt;
#endif
  }

  void postEvent(const PlayerEvent& ev) {
    std::lock_guard<std::mutex> lock(eventMutex);
    if (ev.type == PlayerEventType::SeekRequest) {
      for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (it->type == PlayerEventType::SeekRequest) {
          *it = ev;
          eventCv.notify_one();
          return;
        }
      }
    }
    events.push_back(ev);
    eventCv.notify_one();
  }

  void setState(PlayerState next) {
    PlayerState prev = state.load(std::memory_order_relaxed);
    if (prev == next) return;
    state.store(next, std::memory_order_relaxed);
    appendTimingFmt("state_change from=%s to=%s",
                    playerStateName(prev), playerStateName(next));
    bool holdAudio = (next == PlayerState::Opening ||
                      next == PlayerState::Prefill ||
                      next == PlayerState::Priming ||
                      next == PlayerState::Seeking);
    audioSetHold(holdAudio);
  }

  void clearCurrentFrame() {
    std::lock_guard<std::mutex> lock(currentFrameMutex);
    currentFrame = VideoFrame{};
    hasFrame.store(false, std::memory_order_relaxed);
  }

  playback_clock::Snapshot masterClockSnapshot(int64_t nowUs) const {
    return playback_clock::sample(
        audioStartOk.load(std::memory_order_relaxed) && !audioIsFinished(),
        serialControl.currentSerial(), videoClock, nowUs);
  }

  playback_state_policy::Snapshot statePolicySnapshot(bool audioPaused) const {
    playback_state_policy::Snapshot snapshot;
    snapshot.currentState = state.load(std::memory_order_relaxed);
    snapshot.initDone = initDone.load(std::memory_order_relaxed);
    snapshot.initOk = initOk.load(std::memory_order_relaxed);
    snapshot.audioPaused = audioPaused;
    snapshot.audioStartedOk = audioStartOk.load(std::memory_order_relaxed);
    snapshot.audioFinished = audioIsFinished();
    snapshot.audioDecodeEnded = audioDecodeEnded.load(std::memory_order_relaxed);
    snapshot.decodeEnded = decodeEnded.load(std::memory_order_relaxed);
    snapshot.demuxEnded = demuxEnded.load(std::memory_order_relaxed);
    snapshot.videoQueueDepth = videoFrames.size();
    snapshot.videoQueueEmpty = snapshot.videoQueueDepth == 0;
    snapshot.audioBufferedFrames = audioStreamBufferedFrames();
    AudioPerfStats audioStats = audioGetPerfStats();
    snapshot.audioSampleRate = audioStats.sampleRate;
    snapshot.audioSyncPointReady = audioStreamOldestPtsUs() != AV_NOPTS_VALUE;
    snapshot.currentSerial = serialControl.currentSerial();
    snapshot.lastPresentedSerial =
        lastPresentedSerial.load(std::memory_order_relaxed);
    snapshot.pendingSeekSerial = serialControl.pendingSeekSerial();
    snapshot.seekInFlightSerial = serialControl.seekInFlightSerial();
    snapshot.seekFailed = serialControl.seekFailed();
    return snapshot;
  }

  int64_t masterClockUs() const {
    playback_clock::Snapshot snap = masterClockSnapshot(nowUs());
    return snap.us;
  }

  bool isAudioOk() const { return audioStartOk.load(); }
  bool isAudioStarting() const { return !audioStartDone.load(); }

  void stopThreads() {
    appendTimingFmt("stop_threads begin");
    running.store(false);
    if (frameReadyEvent) {
      SetEvent(frameReadyEvent);
    }
    frameCv.notify_all();
    commandPending.store(false);
    videoPackets.abortQueue();
    audioPackets.abortQueue();
    videoFrames.abort();
    if (demuxThread.joinable()) {
      appendTimingFmt("stop_threads demux_join_begin");
      demuxThread.join();
      appendTimingFmt("stop_threads demux_join_end");
    }
    if (videoDecodeThread.joinable()) {
      appendTimingFmt("stop_threads video_decode_join_begin");
      videoDecodeThread.join();
      appendTimingFmt("stop_threads video_decode_join_end");
    }
    if (audioDecodeThread.joinable()) {
      appendTimingFmt("stop_threads audio_decode_join_begin");
      audioDecodeThread.join();
      appendTimingFmt("stop_threads audio_decode_join_end");
    }
    if (videoOutputThread.joinable()) {
      appendTimingFmt("stop_threads video_output_join_begin");
      videoOutputThread.join();
      appendTimingFmt("stop_threads video_output_join_end");
    }
    videoPackets.flush();
    audioPackets.flush();
    videoFrames.flush(static_cast<uint64_t>(serialControl.currentSerial()));
    destroyVideoDecoder(&videoDec);
    destroyAudioDecoder(&audioDec);
    if (demux.fmt) {
      avformat_close_input(&demux.fmt);
    }
    if (demux.avio) {
      avio_context_free(&demux.avio);
    }
    if (demux.io) {
      demux.io->stop();
      demux.io.reset();
    }
    demux = DemuxContext{};
    appendTimingFmt("stop_threads end");
  }

  void resetControlState() {
    state.store(PlayerState::Idle, std::memory_order_relaxed);
    pauseRequested.store(false, std::memory_order_relaxed);
    serialControl.reset();
    clearFrameRequested.store(false, std::memory_order_relaxed);
    audioBufferedStartPtsUs.store(0, std::memory_order_relaxed);
    audioBufferedStartSerial.store(0, std::memory_order_relaxed);
    audioBufferedStartValid.store(false, std::memory_order_relaxed);
    activeAudioTrack.store(-1, std::memory_order_relaxed);
    activeAudioStream.store(-1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(audioTrackMutex);
      audioTrackStreams.clear();
      audioTrackLabels.clear();
    }
    {
      std::lock_guard<std::mutex> lock(eventMutex);
      events.clear();
    }
  }

  void startThreads() {
    running.store(true);
    decodeEnded.store(false);
    demuxEnded.store(false);
    audioDecodeEnded.store(false);
    initDone.store(false);
    initOk.store(false);
    {
      std::lock_guard<std::mutex> lock(initMutex);
      initError.clear();
    }
    serialControl.startSession(1);
    videoClock.reset(1);
    lastPresentedSerial.store(0);
    lastPresentedPtsUs.store(0);
    lastPresentedDurationUs.store(0);
    frameCounter.store(0);
    lastReadCounter.store(0);
    if (frameReadyEvent) {
      ResetEvent(frameReadyEvent);
    }
    clearCurrentFrame();
    videoPackets.init(32 * 1024 * 1024);
    audioPackets.init(8 * 1024 * 1024);
    videoFrames.init(32);
    videoFrames.flush(static_cast<uint64_t>(serialControl.currentSerial()));
    demuxInterrupt.running = &running;
    demuxInterrupt.commandPending = &commandPending;
    demuxInterrupt.currentSerial = serialControl.currentSerialAtomic();
    demuxInterrupt.lastSerial = serialControl.currentSerial();

    demuxThread = std::thread([this]() { demuxMain(); });
  }

    void handleEvent(const PlayerEvent& ev) {
      auto beginSerialTransition = [&](int64_t targetUs, const char* tag) {
        playback_serial_control::TransitionPlan plan =
            serialControl.beginTransition(
                targetUs, initDone.load(std::memory_order_relaxed),
                running.load(std::memory_order_relaxed));
        if (!plan.valid) return;
        if (plan.signalCommandPending) {
          commandPending.store(true, std::memory_order_relaxed);
        }
        clearFrameRequested.store(true, std::memory_order_relaxed);
        videoClock.reset(plan.serial);
        videoPackets.flush(static_cast<uint64_t>(plan.serial));
        audioPackets.flush(static_cast<uint64_t>(plan.serial));
        videoFrames.flush(static_cast<uint64_t>(plan.serial));
        audioStreamFlushSerial(plan.serial);
        audioBufferedStartPtsUs.store(0, std::memory_order_relaxed);
        audioBufferedStartSerial.store(0, std::memory_order_relaxed);
        audioBufferedStartValid.store(false, std::memory_order_relaxed);
        setState(PlayerState::Seeking);
        appendTimingFmt("%s serial=%d target_us=%lld seek_target_us=%lld",
                        tag ? tag : "ctrl_seek_request", plan.serial,
                        static_cast<long long>(plan.displayTargetUs),
                        static_cast<long long>(plan.demuxTargetUs));
      };

      switch (ev.type) {
        case PlayerEventType::SeekRequest: {
          beginSerialTransition(ev.arg1, "ctrl_seek_request");
          break;
        }
        case PlayerEventType::PauseRequest: {
          pauseRequested.store(ev.arg1 != 0, std::memory_order_relaxed);
          appendTimingFmt("ctrl_pause_request paused=%d",
                          ev.arg1 != 0 ? 1 : 0);
        break;
      }
      case PlayerEventType::ResizeRequest: {
        resizeTargetW.store(static_cast<int>(ev.arg1), std::memory_order_relaxed);
        resizeTargetH.store(static_cast<int>(ev.arg2), std::memory_order_relaxed);
        resizeEpoch.fetch_add(1, std::memory_order_relaxed);
          appendTimingFmt("ctrl_resize_request w=%d h=%d",
                          static_cast<int>(ev.arg1),
                          static_cast<int>(ev.arg2));
          break;
        }
        case PlayerEventType::CycleAudioTrack: {
          int nextTrack = -1;
          int nextStream = -1;
          std::string nextLabel;
          {
            std::lock_guard<std::mutex> lock(audioTrackMutex);
            if (audioTrackStreams.size() <= 1) break;
            int currentTrack = activeAudioTrack.load(std::memory_order_relaxed);
            if (currentTrack < 0 ||
                currentTrack >= static_cast<int>(audioTrackStreams.size())) {
              currentTrack = 0;
            }
            nextTrack = (currentTrack + 1) %
                        static_cast<int>(audioTrackStreams.size());
            nextStream = audioTrackStreams[static_cast<size_t>(nextTrack)];
            if (nextStream < 0) break;
            activeAudioTrack.store(nextTrack, std::memory_order_relaxed);
            activeAudioStream.store(nextStream, std::memory_order_relaxed);
            if (nextTrack >= 0 &&
                nextTrack < static_cast<int>(audioTrackLabels.size())) {
              nextLabel = audioTrackLabels[static_cast<size_t>(nextTrack)];
            }
          }
          beginSerialTransition(masterClockUs(), "ctrl_audio_track_switch");
          appendTimingFmt("audio_track_switch track=%d stream=%d label=%s",
                          nextTrack, nextStream,
                          nextLabel.empty() ? "N/A" : nextLabel.c_str());
          break;
        }
        case PlayerEventType::CloseRequest: {
          ctrlRunning.store(false, std::memory_order_relaxed);
          running.store(false, std::memory_order_relaxed);
        commandPending.store(true, std::memory_order_relaxed);
        appendTiming("ctrl_close_request");
        break;
      }
      case PlayerEventType::SeekApplied: {
        if (serialControl.applySeekResult(ev.serial, static_cast<int>(ev.arg1))) {
          appendTimingFmt("ctrl_seek_applied serial=%d res=%lld", ev.serial,
                          static_cast<long long>(ev.arg1));
        }
        break;
      }
      case PlayerEventType::FirstFramePresented: {
        if (ev.serial == serialControl.currentSerial()) {
          if (state.load(std::memory_order_relaxed) == PlayerState::Priming) {
            int64_t audioOldestPts = audioStreamOldestPtsUs();
            int64_t videoPts = lastPresentedPtsUs.load(std::memory_order_relaxed);

            playback_sync::PrimingAnchor anchor =
                playback_sync::choosePrimingAnchor(audioOldestPts, videoPts);
            if (anchor.valid) {
              int64_t now = nowUs();

              playback_clock::synchronizePrimingClocks(ev.serial, anchor.ptsUs,
                                                       videoClock, now);

              appendTimingFmt(
                  "sync_priming_end serial=%d sync_pts_us=%lld audio_oldest=%lld video_pts=%lld",
                  ev.serial, static_cast<long long>(anchor.ptsUs),
                  static_cast<long long>(audioOldestPts),
                  static_cast<long long>(videoPts));
            }

            audioBufferedStartPtsUs.store(0, std::memory_order_relaxed);
            audioBufferedStartSerial.store(0, std::memory_order_relaxed);
            audioBufferedStartValid.store(false, std::memory_order_relaxed);
            setState(PlayerState::Playing);
          }
        }
        break;
      }
    }
  }

  void controlMain() {
    resetControlState();
    setState(PlayerState::Opening);
    startThreads();
    while (ctrlRunning.load()) {
      std::deque<PlayerEvent> pending;
      {
        std::unique_lock<std::mutex> lock(eventMutex);
        eventCv.wait_for(lock, std::chrono::milliseconds(10), [&]() {
          return !events.empty() || !ctrlRunning.load();
        });
        pending.swap(events);
      }
      for (const auto& ev : pending) {
        handleEvent(ev);
      }

      const bool audioPaused =
          audioStartOk.load() ? audioIsPaused() : pauseRequested.load();
      playback_state_policy::Snapshot snapshot = statePolicySnapshot(audioPaused);
      PlayerState resolvedState = playback_state_policy::resolveSteadyState(
          snapshot, kVideoPrefillFrames);
      if (resolvedState != snapshot.currentState) {
        if (snapshot.currentState == PlayerState::Seeking &&
            snapshot.seekInFlightSerial == 0) {
          serialControl.clearSeekFailure();
        }
        setState(resolvedState);
      }
    }
    setState(PlayerState::Closing);
    appendTimingFmt("control_main stop_threads_begin");
    stopThreads();
    appendTimingFmt("control_main stop_threads_end");
    setState(PlayerState::Idle);
  }

  void demuxMain() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInit = SUCCEEDED(hr);
    HRESULT roHr = RoInitialize(RO_INIT_MULTITHREADED);
    bool roInit = SUCCEEDED(roHr);

    std::string error;
    demux = DemuxContext{};
    videoDec = VideoDecodeContext{};
    audioDec = AudioDecodeContext{};

    auto failInit = [&](const std::string& message) {
      {
        std::lock_guard<std::mutex> lock(initMutex);
        initError = message;
      }
      initOk.store(false);
      initDone.store(true);
      audioStartOk.store(false);
      audioStartDone.store(true);
      running.store(false);
      videoPackets.abortQueue();
      audioPackets.abortQueue();
      videoFrames.abort();
      if (roInit) {
        RoUninitialize();
      }
      if (comInit) {
        CoUninitialize();
      }
    };

    if (!initDemuxer(config.file, &demux, &error, &demuxInterrupt)) {
      if (demux.fmt) {
        avformat_close_input(&demux.fmt);
      }
      failInit(error.empty() ? "Failed to open video." : error);
      return;
    }
    if (demux.videoStreamIndex < 0) {
      failInit("No video stream found.");
      return;
    }
    if (!initVideoDecoder(demux, true, &videoDec, &error)) {
      failInit(error.empty() ? "Failed to open video decoder." : error);
      return;
    }

    if (config.allowDecoderScale && !videoDec.useSharedDevice) {
      if (config.targetWidth > 0 && config.targetHeight > 0) {
        videoDec.targetW = config.targetWidth;
        videoDec.targetH = config.targetHeight;
        clampTargetSize(videoDec.targetW, videoDec.targetH);
      }
    }

      sourceWidth.store(videoDec.width);
      sourceHeight.store(videoDec.height);
      durationUs.store(demux.durationUs > 0 ? demux.durationUs : 0);
      {
        std::lock_guard<std::mutex> lock(audioTrackMutex);
        audioTrackStreams = demux.audioStreamIndices;
        audioTrackLabels = demux.audioTrackLabels;
      }
      activeAudioTrack.store(demux.activeAudioTrack, std::memory_order_relaxed);
      activeAudioStream.store(demux.audioStreamIndex, std::memory_order_relaxed);

    double estimatedFrameDurationSec = 0.0;
    if (demux.videoStreamIndex >= 0 && demux.fmt) {
      AVStream* stream = demux.fmt->streams[demux.videoStreamIndex];
      if (stream) {
        AVRational rate = av_guess_frame_rate(demux.fmt, stream, nullptr);
        if (rate.num > 0 && rate.den > 0) {
          estimatedFrameDurationSec = av_q2d(av_inv_q(rate));
        } else if (stream->avg_frame_rate.num > 0 &&
                   stream->avg_frame_rate.den > 0) {
          estimatedFrameDurationSec = av_q2d(av_inv_q(stream->avg_frame_rate));
        } else if (stream->r_frame_rate.num > 0 &&
                   stream->r_frame_rate.den > 0) {
          estimatedFrameDurationSec = av_q2d(av_inv_q(stream->r_frame_rate));
        }
      }
    }
    if (estimatedFrameDurationSec <= 0.0) {
      estimatedFrameDurationSec = 1.0 / 30.0;
    }
    fallbackFrameDurationUs.store(
        static_cast<int64_t>(estimatedFrameDurationSec * 1000000.0));

    initOk.store(true);
    initDone.store(true);

    videoDecodeThread = std::thread([this]() { videoDecodeMain(); });
    videoOutputThread = std::thread([this]() { videoOutputMain(); });
    if (config.enableAudio && demux.audioStreamIndex >= 0) {
      audioDecodeThread = std::thread([this]() { audioDecodeMain(); });
    } else {
      audioStartOk.store(false);
      audioStartDone.store(true);
      audioDecodeEnded.store(true);
    }

    uint64_t serial = static_cast<uint64_t>(serialControl.currentSerial());
    AVPacket pkt{};
    bool demuxAtEof = false;
    
    // Packet queue backpressure monitoring
    int64_t lastQueueWarnTimeUs = nowUs();
    constexpr int64_t kQueueWarnIntervalUs = 5000000;  // 5 seconds between warnings
    constexpr size_t kMaxPacketQueueSize = 4096;  // Increased to avoid head-of-line blocking
    
    while (running.load()) {
      bool handledSeek = false;
      int nextSerialValue = serialControl.currentSerial();
      if (nextSerialValue != static_cast<int>(serial) &&
          !serialControl.seekPending()) {
        serial = static_cast<uint64_t>(nextSerialValue);
        demuxInterrupt.lastSerial = nextSerialValue;
        // If serial changed externally (stall recovery), we should flush to be safe
        videoPackets.flush(serial);
        audioPackets.flush(serial);
        videoFrames.flush(serial);
        appendTimingFmt("demux_external_serial_sync serial=%d", nextSerialValue);
      }

      playback_serial_control::PendingSeek pendingSeek =
          serialControl.claimPendingSeek();
      if (pendingSeek.valid) {
        int nextSerial = pendingSeek.serial;
        demuxInterrupt.lastSerial = nextSerial;
        commandPending.store(false);

        int64_t targetUs = pendingSeek.demuxTargetUs;
        targetUs += demux.formatStartUs;
        if (targetUs < 0) targetUs = 0;
        appendTimingFmt("demux_seek serial=%d target_us=%lld", nextSerial,
                        static_cast<long long>(targetUs));
        int seekRes = avformat_seek_file(demux.fmt, -1, INT64_MIN, targetUs,
                                         INT64_MAX, AVSEEK_FLAG_BACKWARD);
        if (seekRes < 0 && demux.videoStreamIndex >= 0) {
          int64_t targetStream =
              av_rescale_q(targetUs, AVRational{1, AV_TIME_BASE},
                           demux.videoTimeBase);
          seekRes = av_seek_frame(demux.fmt, demux.videoStreamIndex,
                                  targetStream, AVSEEK_FLAG_BACKWARD);
        }
        appendTimingFmt("demux_seek_result serial=%d res=%d", nextSerial,
                        seekRes);
        if (seekRes < 0) {
          appendTiming("demux_seek_failed");
        } else {
          avformat_flush(demux.fmt);
        }
        serial = static_cast<uint64_t>(nextSerial);
        demuxEnded.store(false);
        demuxAtEof = false;
        decodeEnded.store(false);
        audioDecodeEnded.store(false);
        videoPackets.flush(serial);
        audioPackets.flush(serial);
        videoFrames.flush(serial);
        audioStreamFlushSerial(static_cast<int>(serial));
        videoClock.reset(static_cast<int>(serial));
        clearFrameRequested.store(true);
        PlayerEvent ev{};
        ev.type = PlayerEventType::SeekApplied;
        ev.serial = nextSerial;
        ev.arg1 = (seekRes < 0) ? 1 : 0;
        postEvent(ev);
        handledSeek = true;
      }

      if (!serialControl.seekPending()) {
        commandPending.store(false);
      }

      if (handledSeek) {
        continue;
      }

      if (demuxAtEof) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      int read = av_read_frame(demux.fmt, &pkt);
      if (read == AVERROR_EXIT || read == AVERROR(EINTR)) {
        if (!running.load()) {
          break;
        }
        continue;
      }
      if (read == AVERROR(EAGAIN)) {
        continue;
      }
      if (read < 0) {
        if (read == AVERROR_EOF) {
          if (!demuxAtEof) {
            demuxEnded.store(true);
            videoPackets.pushEof(serial);
            audioPackets.pushEof(serial);
            appendTiming("demux_read_end_eof");
            demuxAtEof = true;
          }
        } else {
          // Non-EOF error (e.g. corruption, network timeout, bitstream error)
          static int lastErr = 0;
          if (read != lastErr) {
            appendTimingFmt("demux_read_error err=%d", read);
            lastErr = read;
          }
          // Just wait a bit and retry. If it's a real gap, the output thread stall logic will jump us over it.
          // This prevents "demux_read_end" from stopping the demuxer on transient errors.
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        continue;
      }

        int selectedAudioStream =
            activeAudioStream.load(std::memory_order_relaxed);
        if (pkt.stream_index == demux.videoStreamIndex) {
        // Always allow blocking to prevent packet drops (which cause gaps).
        // Deadlock risk is handled by appropriate queue sizing and master clock.
        bool allowBlock = true; 
        bool queued = false;
        if (!videoPackets.pushPacket(&pkt, serial, allowBlock, &commandPending,
                                     &queued)) {
          av_packet_unref(&pkt);
          break;
        }
        if (!queued) {
          // Should not happen with allowBlock=true unless aborted
          av_packet_unref(&pkt);
          continue;
        }
        } else if (selectedAudioStream >= 0 &&
                   pkt.stream_index == selectedAudioStream) {
        bool allowBlock = true;
        bool queued = false;
        if (!audioPackets.pushPacket(&pkt, serial, allowBlock, &commandPending,
                                     &queued)) {
          av_packet_unref(&pkt);
          break;
        }
        if (!queued) {
          av_packet_unref(&pkt);
          continue;
        }
      }
      av_packet_unref(&pkt);
      
      // Packet queue backpressure detection:
      // Warn if queues are growing (demuxer reading faster than decoders consuming)
      size_t vqSize = videoPackets.size();
      size_t aqSize = audioPackets.size();
      int64_t now = nowUs();
      if (vqSize > kMaxPacketQueueSize || aqSize > kMaxPacketQueueSize) {
        if (now - lastQueueWarnTimeUs > kQueueWarnIntervalUs) {
          appendTimingFmt("packet_queue_backpressure video_q=%zu audio_q=%zu",
                          vqSize, aqSize);
          lastQueueWarnTimeUs = now;
          // If audio queue is large and video queue is small, audio decoder might be stuck
          if (aqSize > kMaxPacketQueueSize && vqSize < kMaxPacketQueueSize / 2) {
            appendTimingFmt("possible_audio_stuck audio_q=%zu video_q=%zu", aqSize, vqSize);
          }
        }
      } else {
        lastQueueWarnTimeUs = now;  // Reset timer when queues are healthy
      }
    }

    demuxEnded.store(true);
    if (roInit) {
      RoUninitialize();
    }
    if (comInit) {
      CoUninitialize();
    }
  }

  void videoDecodeMain() {
    HRESULT vhr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool vcomInit = SUCCEEDED(vhr);
    HRESULT vroHr = RoInitialize(RO_INIT_MULTITHREADED);
    bool vroInit = SUCCEEDED(vroHr);

    uint64_t decoderSerial = 0;
    uint64_t lastResizeEpoch = 0;
    bool inputEof = false;
    int64_t lastPtsUs = 0;
    int64_t lastDurationUs =
        (fallbackFrameDurationUs.load() > 0) ? fallbackFrameDurationUs.load()
                                             : 33333;
    QueuedPacket pendingPacket{};
    bool hasPendingPacket = false;

    while (running.load()) {
      uint64_t newResizeEpoch = resizeEpoch.load();
      if (newResizeEpoch != lastResizeEpoch) {
        int targetW = resizeTargetW.load();
        int targetH = resizeTargetH.load();
        if (targetW > 0 && targetH > 0) {
          videoDec.targetW = targetW;
          videoDec.targetH = targetH;
          if (!videoDec.useSharedDevice) {
            clampTargetSize(videoDec.targetW, videoDec.targetH);
          } else if (videoDec.rotationQuarterTurns != 0) {
            normalizeEvenTargetSize(videoDec.targetW, videoDec.targetH);
          }
        }
        lastResizeEpoch = newResizeEpoch;
        videoFrames.flush(static_cast<uint64_t>(serialControl.currentSerial()));
      }

      if (!hasPendingPacket) {
        if (!videoPackets.pop(&pendingPacket)) {
          break;
        }
        hasPendingPacket = true;
      }

      if (pendingPacket.flush) {
        avcodec_flush_buffers(videoDec.codec);
        decoderSerial = pendingPacket.serial;
        inputEof = false;
        hasPendingPacket = false;
        videoFrames.flush(decoderSerial);
        lastPtsUs = 0;
        lastDurationUs =
            (fallbackFrameDurationUs.load() > 0) ? fallbackFrameDurationUs.load()
                                                 : 33333;
        continue;
      }

      if (decoderSerial == 0) {
        decoderSerial = pendingPacket.serial;
      }
      if (pendingPacket.serial != decoderSerial) {
        av_packet_unref(&pendingPacket.pkt);
        hasPendingPacket = false;
        continue;
      }

      if (pendingPacket.eof) {
        avcodec_send_packet(videoDec.codec, nullptr);
        hasPendingPacket = false;
        inputEof = true;
      } else {
        int send = avcodec_send_packet(videoDec.codec, &pendingPacket.pkt);
        if (send != AVERROR(EAGAIN)) {
          av_packet_unref(&pendingPacket.pkt);
          hasPendingPacket = false;
          if (send < 0 && send != AVERROR_EOF) {
            continue;
          }
        }
      }

      while (running.load()) {
        // Break out and flush if the master serial has changed (e.g., requested by output thread stall recovery)
        int currentMasterSerial = serialControl.currentSerial();
        if (decoderSerial != 0 && decoderSerial != static_cast<uint64_t>(currentMasterSerial)) {
          avcodec_flush_buffers(videoDec.codec);
          hasPendingPacket = false;
          decoderSerial = static_cast<uint64_t>(currentMasterSerial); // Sync to new serial
          videoFrames.flush(decoderSerial);
          break;
        }

        // HEARTBEAT DECODER
        static int64_t lastDecTickUs = 0;
        int64_t nowDec = nowUs();
        if (nowDec - lastDecTickUs > 1000000) {
          appendTimingFmt("video_heartbeat_decoder q=%zu has_pkt=%d eof=%d", 
                          videoFrames.size(), hasPendingPacket ? 1 : 0, inputEof ? 1 : 0);
          lastDecTickUs = nowDec;
        }

        int recv = avcodec_receive_frame(videoDec.codec, videoDec.frame);

        if (recv == AVERROR(EAGAIN)) {
          break;
        }
        if (recv == AVERROR_EOF) {
          if (inputEof) {
            decodeEnded.store(true);
          }
          break;
        }
        if (recv < 0) {
          decodeEnded.store(true);
          break;
        }

        size_t poolIndex = 0;
        size_t queueLimit = maxQueue.load(std::memory_order_relaxed);
        if (queueLimit < 1) queueLimit = 1;
        if (queueLimit > 32) queueLimit = 32;
        if (!videoFrames.acquireFrame(&poolIndex, decoderSerial, queueLimit,
                                      &running)) {
          av_frame_unref(videoDec.frame);
          break;
        }

        VideoFrame& decodedFrame = videoFrames.frame(poolIndex);
        VideoReadInfo info{};
        int srcFmt = videoDec.frame ? videoDec.frame->format : -1;
        int srcW = videoDec.frame ? videoDec.frame->width : 0;
        int srcH = videoDec.frame ? videoDec.frame->height : 0;
        auto decodeStart = std::chrono::steady_clock::now();
        bool ok = emitVideoFrame(&videoDec, decodedFrame, &info,
                                 videoDec.frame, true,
                                 videoDec.useSharedDevice);
        av_frame_unref(videoDec.frame);
        if (!ok) {
          appendTimingFmt(
              "video_emit_failed serial=%u src_fmt=%d src=%dx%d target=%dx%d rot=%d shared=%d",
              static_cast<unsigned>(decoderSerial), srcFmt, srcW, srcH,
              videoDec.targetW, videoDec.targetH, videoDec.rotationQuarterTurns,
              videoDec.useSharedDevice ? 1 : 0);
          videoFrames.release(poolIndex);
          continue;
        }

        auto decodeEnd = std::chrono::steady_clock::now();
        double decodeMs =
            std::chrono::duration<double, std::milli>(decodeEnd - decodeStart)
                .count();

        int64_t ptsUs = 0;
        if (decodedFrame.timestamp100ns > 0) {
          ptsUs = decodedFrame.timestamp100ns / 10;
        } else if (lastPtsUs > 0) {
          ptsUs = lastPtsUs + lastDurationUs;
        }
        int64_t frameDurationUs = 0;
        if (info.duration100ns > 0) {
          frameDurationUs = info.duration100ns / 10;
        } else {
          frameDurationUs = lastDurationUs;
        }
        if (frameDurationUs <= 0) {
          frameDurationUs = (fallbackFrameDurationUs.load() > 0)
                                ? fallbackFrameDurationUs.load()
                                : 33333;
        }
        lastPtsUs = ptsUs;
        lastDurationUs = frameDurationUs;

        videoFrames.push(QueuedFrame{poolIndex, ptsUs, frameDurationUs,
                                     static_cast<uint64_t>(decoderSerial), info,
                                     decodeMs});
      }
    }

    decodeEnded.store(true);
    if (vroInit) {
      RoUninitialize();
    }
    if (vcomInit) {
      CoUninitialize();
    }
  }

    void audioDecodeMain() {
      int decoderStreamIndex =
          activeAudioStream.load(std::memory_order_relaxed);
      uint32_t outRate = 48000;
      uint32_t outChannels = 2;
      bool audioReady = false;
      if (config.enableAudio && decoderStreamIndex >= 0) {
        uint64_t totalFrames = 0;
        AVStream* stream =
            (demux.fmt && decoderStreamIndex < static_cast<int>(demux.fmt->nb_streams))
                ? demux.fmt->streams[decoderStreamIndex]
                : nullptr;
        if (stream && stream->duration > 0) {
          int64_t us = av_rescale_q(stream->duration, stream->time_base,
                                    AVRational{1, AV_TIME_BASE});
        if (us > 0) {
          totalFrames = static_cast<uint64_t>(
              rescaleToFrames(us, AVRational{1, AV_TIME_BASE}, 48000));
        }
      } else if (demux.durationUs > 0) {
        totalFrames = static_cast<uint64_t>(
            rescaleToFrames(demux.durationUs, AVRational{1, AV_TIME_BASE},
                            48000));
      }
        if (audioStartStream(totalFrames)) {
          audioStreamFlushSerial(serialControl.currentSerial());
          AudioPerfStats stats = audioGetPerfStats();
          outRate = stats.sampleRate ? stats.sampleRate : 48000;
          outChannels = stats.channels ? stats.channels : 2;
          std::string error;
          if (initAudioDecoder(demux, decoderStreamIndex, outRate, outChannels,
                               &audioDec, &error)) {
            audioReady = true;
          } else {
            appendTimingFmt("audio_init_failed msg=%s", error.c_str());
          audioStopStream();
        }
      } else {
        appendTiming("audio_start_failed");
      }
    }
    audioStartOk.store(audioReady);
    audioStartDone.store(true);
      if (!audioReady) {
        audioDecodeEnded.store(true);
        return;
      }

      auto reinitializeDecoderForActiveTrack = [&]() -> bool {
        int desiredStream = activeAudioStream.load(std::memory_order_relaxed);
        if (desiredStream == decoderStreamIndex) {
          return true;
        }
        if (!demux.fmt || desiredStream < 0 ||
            desiredStream >= static_cast<int>(demux.fmt->nb_streams)) {
          appendTimingFmt("audio_track_switch_failed stream=%d", desiredStream);
          return false;
        }
        AudioDecodeContext nextAudioDec{};
        std::string error;
        if (!initAudioDecoder(demux, desiredStream, outRate, outChannels,
                              &nextAudioDec, &error)) {
          appendTimingFmt("audio_track_init_failed stream=%d msg=%s",
                          desiredStream, error.c_str());
          destroyAudioDecoder(&nextAudioDec);
          return false;
        }
        destroyAudioDecoder(&audioDec);
        audioDec = std::move(nextAudioDec);
        decoderStreamIndex = desiredStream;
        appendTimingFmt("audio_track_decoder_reset stream=%d", desiredStream);
        return true;
      };

      // Timeout detection for audio decoder stall
      int64_t lastAudioFrameTimeUs = nowUs();
      constexpr int64_t kAudioStallThresholdUs = 5000000;  // 5 seconds

    QueuedPacket pendingPacket{};
    bool hasPendingPacket = false;
    bool inputEof = false;
    uint64_t decoderSerial = 0;
    while (running.load()) {
      if (!hasPendingPacket) {
        if (!audioPackets.pop(&pendingPacket)) {
          break;
        }
        hasPendingPacket = true;
        }

        if (pendingPacket.flush) {
          if (!reinitializeDecoderForActiveTrack()) {
            audioStartOk.store(false, std::memory_order_relaxed);
            break;
          }
          avcodec_flush_buffers(audioDec.codec);
          audioDec.writePosFrames = 0;
          audioDec.nextPtsUs = 0;
        audioDec.nextPtsValid = false;
        decoderSerial = pendingPacket.serial;
        audioStreamFlushSerial(static_cast<int>(decoderSerial));
        hasPendingPacket = false;
        inputEof = false;
        continue;
      }

      if (decoderSerial == 0) {
        decoderSerial = pendingPacket.serial;
      }
      if (pendingPacket.serial != decoderSerial) {
        av_packet_unref(&pendingPacket.pkt);
        hasPendingPacket = false;
        continue;
      }

      if (pendingPacket.eof) {
        avcodec_send_packet(audioDec.codec, nullptr);
        hasPendingPacket = false;
        inputEof = true;
      } else {
        int send = avcodec_send_packet(audioDec.codec, &pendingPacket.pkt);
        if (send != AVERROR(EAGAIN)) {
          av_packet_unref(&pendingPacket.pkt);
          hasPendingPacket = false;
          if (send < 0 && send != AVERROR_EOF) {
            continue;
          }
        }
      }

      while (running.load()) {
        // Break out and flush if the master serial has changed
        int currentMasterSerial = serialControl.currentSerial();
        if (decoderSerial != 0 && decoderSerial != static_cast<uint64_t>(currentMasterSerial)) {
          avcodec_flush_buffers(audioDec.codec);
          hasPendingPacket = false;
          decoderSerial = static_cast<uint64_t>(currentMasterSerial);
          audioStreamFlushSerial(static_cast<int>(decoderSerial));
          break;
        }

        int recv = avcodec_receive_frame(audioDec.codec, audioDec.frame);
        if (recv == AVERROR(EAGAIN)) {
          break;
        }
        if (recv == AVERROR_EOF) {
          if (inputEof) {
            audioStreamSetEnd(true);
            audioDecodeEnded.store(true);
          }
          break;
        }
        if (recv < 0) {
          audioStreamSetEnd(true);
          audioDecodeEnded.store(true);
          break;
        }

        int64_t pts = audioDec.frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) {
          pts = audioDec.frame->pts;
        }
        if (pts == AV_NOPTS_VALUE) {
          pts = audioDec.frame->pkt_dts;
        }

        int64_t outSamples =
            swr_get_out_samples(audioDec.swr, audioDec.frame->nb_samples);
        if (outSamples <= 0) {
          av_frame_unref(audioDec.frame);
          continue;
        }
        audioDec.convertBuffer.resize(
            static_cast<size_t>(outSamples) * audioDec.outChannels);
        uint8_t* outData =
            reinterpret_cast<uint8_t*>(audioDec.convertBuffer.data());
        int converted =
            swr_convert(audioDec.swr, &outData, static_cast<int>(outSamples),
                        const_cast<const uint8_t**>(audioDec.frame->data),
                        audioDec.frame->nb_samples);
        av_frame_unref(audioDec.frame);
        if (converted <= 0) {
          continue;
        }
        int64_t ptsUs = AV_NOPTS_VALUE;
        if (pts != AV_NOPTS_VALUE) {
          int64_t absUs = ptsToUs(pts, audioDec.timeBase);
          if (absUs != AV_NOPTS_VALUE) {
            ptsUs = absUs - audioDec.formatStartUs;
            if (ptsUs < 0) ptsUs = 0;
          }
        }
        if (audioDec.pendingSkipFrames > 0) {
          uint64_t skipFrames =
              std::min<uint64_t>(audioDec.pendingSkipFrames,
                                 static_cast<uint64_t>(converted));
          audioDec.pendingSkipFrames -= skipFrames;
          if (skipFrames >= static_cast<uint64_t>(converted)) {
            if (ptsUs == AV_NOPTS_VALUE) {
              int64_t skipUs = static_cast<int64_t>(
                  (skipFrames * 1000000ULL) / audioDec.outRate);
              if (audioDec.nextPtsValid) {
                audioDec.nextPtsUs += skipUs;
              } else {
                audioDec.writePosFrames += static_cast<int64_t>(skipFrames);
              }
            }
            continue;
          }
          if (skipFrames > 0) {
            size_t remaining =
                static_cast<size_t>(converted - skipFrames);
            std::memmove(audioDec.convertBuffer.data(),
                         audioDec.convertBuffer.data() +
                             skipFrames * audioDec.outChannels,
                         remaining * audioDec.outChannels * sizeof(float));
            converted = static_cast<int>(remaining);
            int64_t skipUs = static_cast<int64_t>(
                (skipFrames * 1000000ULL) / audioDec.outRate);
            if (ptsUs != AV_NOPTS_VALUE) {
              ptsUs += skipUs;
            } else if (audioDec.nextPtsValid) {
              audioDec.nextPtsUs += skipUs;
            } else {
              audioDec.writePosFrames += static_cast<int64_t>(skipFrames);
            }
          }
        }
        int64_t audioDurationUs = static_cast<int64_t>(
            (static_cast<uint64_t>(converted) * 1000000ULL) /
            static_cast<uint64_t>(audioDec.outRate));
        if (ptsUs == AV_NOPTS_VALUE) {
          if (audioDec.nextPtsValid) {
            ptsUs = audioDec.nextPtsUs;
          } else {
            ptsUs = static_cast<int64_t>(
                (static_cast<uint64_t>(audioDec.writePosFrames) * 1000000ULL) /
                static_cast<uint64_t>(audioDec.outRate));
          }
        }
        audioDec.nextPtsUs = ptsUs + audioDurationUs;
        audioDec.nextPtsValid = true;
        audioDec.writePosFrames += static_cast<int64_t>(converted);
        audioStreamProcessRadio(audioDec.convertBuffer.data(),
                                static_cast<uint32_t>(converted));
        
        uint64_t totalWritten = 0;
        while (running.load() && totalWritten < static_cast<uint64_t>(converted)) {
          if (static_cast<int>(decoderSerial) != serialControl.currentSerial()) {
            break;
          }
          uint64_t written = 0;
          [[maybe_unused]] PlayerState st = state.load(std::memory_order_relaxed);
          // Always allow blocking to ensure we don't discard audio during prefill.
          // The demuxer deadlock is now prevented by prefillReady returning true if video is full.
          bool allowBlock = true;
          
          if (!audioStreamWriteSamples(audioDec.convertBuffer.data() + 
                                       totalWritten * audioDec.outChannels,
                                     static_cast<uint64_t>(converted) - totalWritten, 
                                     ptsUs,
                                     static_cast<int>(decoderSerial),
                                     allowBlock, &written)) {
            break;
          }
          if (written == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
          }
          totalWritten += written;
          
          // Reset audio stall timer on successful frame write
          lastAudioFrameTimeUs = nowUs();
          
          int bufferedSerial =
              audioBufferedStartSerial.load(std::memory_order_relaxed);
          bool firstForSerial =
              (bufferedSerial != static_cast<int>(decoderSerial) ||
               !audioBufferedStartValid.load(std::memory_order_relaxed));
          if (firstForSerial && ptsUs != AV_NOPTS_VALUE) {
            audioBufferedStartSerial.store(static_cast<int>(decoderSerial),
                                           std::memory_order_relaxed);
            audioBufferedStartPtsUs.store(ptsUs, std::memory_order_relaxed);
            audioBufferedStartValid.store(true, std::memory_order_relaxed);
          }
        }
      }
    }
    
    // Audio stall detection: if no frames produced for 5+ seconds, force recovery
    int64_t now = nowUs();
    if (now - lastAudioFrameTimeUs > kAudioStallThresholdUs) {
      appendTimingFmt("audio_stall_detected serial=%u frames_buffered=%zu",
                      static_cast<unsigned>(decoderSerial),
                      audioStreamBufferedFrames());
      // Force recovery: flush decoder and return to prefill
      avcodec_flush_buffers(audioDec.codec);
      audioDec.writePosFrames = 0;
      audioDec.nextPtsUs = 0;
      audioDec.nextPtsValid = false;
      audioStreamFlushSerial(static_cast<int>(decoderSerial));
      lastAudioFrameTimeUs = now;
    }
    
    audioStreamSetEnd(true);
    audioDecodeEnded.store(true);
  }

  void videoOutputMain() {
    PlayerState lastState = state.load();
    constexpr double kVideoBufferHighSec = 0.75;
    playback_sync::LoopState syncState;
    playback_sync::initializeLoopState(
        syncState, serialControl.currentSerial(),
        fallbackFrameDurationUs.load(std::memory_order_relaxed), nowUs());
    auto logVideo = [&](const char* tag, const QueuedFrame* frame,
                        int64_t masterUs, PlayerClockSource source,
                        int64_t diffUs, int64_t delayUs) {
      PlayerState st = state.load(std::memory_order_relaxed);
      size_t q = videoFrames.size();
      int audioReady = audioStreamClockReady() ? 1 : 0;
      int audioFresh = masterClockSnapshot(nowUs()).audioClockFresh ? 1 : 0;
      int audioStarved = audioStreamStarved() ? 1 : 0;

      // Filter out high-frequency logs to keep log file clean
      if (std::strcmp(tag, "present") == 0 || std::strcmp(tag, "queue_high") == 0 || 
          std::strcmp(tag, "queue_low") == 0 || std::strcmp(tag, "drop_serial") == 0 || 
          std::strcmp(tag, "drop_backwards") == 0) {
        // Heartbeat: Log every 30th event (approx 1s) regardless of sync/state
        static int eventCounter = 0;
        bool heartbeat = (++eventCounter % 30 == 0);
        bool syncIssue = std::abs(diffUs) > 100000; // >100ms sync gap
        if (!heartbeat && !syncIssue && (std::strcmp(tag, "present") == 0 || std::strcmp(tag, "drop_serial") == 0 || std::strcmp(tag, "drop_backwards") == 0)) return;
        if (!heartbeat && std::strcmp(tag, "present") != 0 && std::strcmp(tag, "drop_serial") != 0 && std::strcmp(tag, "drop_backwards") != 0) return;
      }
      
      if (frame) {
        appendTimingFmt(
            "video_%s state=%s serial=%d pts_us=%lld dur_us=%lld master=%lld master_src=%s diff_us=%lld delay_us=%lld q=%zu dec_ms=%.1f audio_ready=%d audio_fresh=%d audio_starved=%d",
            tag, playerStateName(st), static_cast<int>(frame->serial),
            static_cast<long long>(frame->ptsUs),
            static_cast<long long>(frame->durationUs),
            static_cast<long long>(masterUs), clockSourceName(source),
            static_cast<long long>(diffUs),
            static_cast<long long>(delayUs), q, frame->decodeMs, audioReady,
            audioFresh, audioStarved);
      } else {
        appendTimingFmt(
            "video_%s state=%s q=%zu master=%lld audio_ready=%d audio_fresh=%d audio_starved=%d",
            tag, playerStateName(st), q, static_cast<long long>(masterUs), audioReady, audioFresh, audioStarved);
      }
    };

    while (running.load()) {
      if (clearFrameRequested.exchange(false)) {
        clearCurrentFrame();
      }

      PlayerState st = state.load();
      if (st != lastState) {
        lastState = st;
        playback_sync::notePlaybackStateChange(syncState, nowUs());
      }

      uint64_t serial = static_cast<uint64_t>(serialControl.currentSerial());
      playback_sync::syncLoopSerial(
          syncState, static_cast<int>(serial),
          fallbackFrameDurationUs.load(std::memory_order_relaxed), nowUs());

      if (st == PlayerState::Seeking) {
        // Seeking is a transient state. While paused, it may present exactly
        // the pending seek frame; active playback must return through Prefill.
        QueuedFrame front{};
        if (!videoFrames.peek(&front)) {
          videoFrames.waitForFrame(std::chrono::milliseconds(5), &running,
                                   &commandPending);
          continue;
        }
        if (front.serial != serial) {
          logVideo("drop_serial", &front, 0, PlayerClockSource::None, 0, 0);
          QueuedFrame drop{};
          if (videoFrames.pop(&drop)) {
            videoFrames.release(drop.poolIndex);
          }
          std::this_thread::sleep_for(std::chrono::microseconds(100));
          continue;
        }
        const bool pausedForPreview =
            audioStartOk.load(std::memory_order_relaxed)
                ? audioIsPaused()
                : pauseRequested.load(std::memory_order_relaxed);
        if (!pausedForPreview ||
            serialControl.pendingSeekSerial() != static_cast<int>(serial)) {
          videoClock.set_paused(true, nowUs());
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          continue;
        }
      }

      if (st != PlayerState::Playing && st != PlayerState::Draining &&
          st != PlayerState::Priming && st != PlayerState::Seeking) {
        videoClock.set_paused(true, nowUs());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      // Always update master clock and diff, even if no frames are in the queue.
      // This ensures the UI reflects current sync status during the "Draining" state.
      int64_t now = nowUs();
      playback_clock::Snapshot master = masterClockSnapshot(now);
      int64_t masterUs = master.us;

      static int64_t lastHeartbeatUs = 0;
      if (now - lastHeartbeatUs > 1000000) {
        logVideo("heartbeat_output", nullptr, masterUs, master.source, 0, 0);
        lastHeartbeatUs = now;
      }

      lastMasterUs.store(masterUs, std::memory_order_relaxed);
      lastMasterSource.store(static_cast<int>(master.source),
                             std::memory_order_relaxed);
      
      int64_t lpPts = lastPresentedPtsUs.load(std::memory_order_relaxed);
      int lpSerial = lastPresentedSerial.load(std::memory_order_relaxed);
      if (masterUs > 0 && lpPts > 0 && lpSerial == static_cast<int>(serial)) {
        lastDiffUs.store(lpPts - masterUs, std::memory_order_relaxed);
      } else {
        lastDiffUs.store(0, std::memory_order_relaxed);
      }

      QueuedFrame front{};
      if (!videoFrames.peek(&front)) {
        if (playback_sync::shouldBackoffForEmptyQueue(
                syncState, st, serialControl.seekPending(),
                durationUs.load(std::memory_order_relaxed),
                lastPresentedPtsUs.load(std::memory_order_relaxed), now)) {
          logVideo("stall_detected_wait", nullptr, 0, PlayerClockSource::None,
                   0, 0);
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          syncState.lastPresentedTimeUs = now;
        }

        videoFrames.waitForFrame(std::chrono::milliseconds(10), &running,
                                 &commandPending);
        continue;
      }

      if (front.serial != serial) {
        logVideo("drop_serial", &front, 0, PlayerClockSource::None, 0, 0);
        QueuedFrame drop{};
        if (videoFrames.pop(&drop)) {
          videoFrames.release(drop.poolIndex);
        }
        // Small sleep to avoid tight-looping while flushing old serials
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        continue;
      }

      if (playback_sync::isFrameBackwards(syncState, front)) {
        logVideo("drop_backwards", &front, 0, PlayerClockSource::None, 0, 0);
        QueuedFrame drop{};
        if (videoFrames.pop(&drop)) {
          videoFrames.release(drop.poolIndex);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        continue;
      }

      QueuedFrame next{};
      bool hasNext = videoFrames.peekNext(&next);
      playback_sync::PreparedFrame prepared =
          playback_sync::prepareFrame(syncState, front,
                                      hasNext ? &next : nullptr);
      if (front.durationUs > 500000) {
        logVideo("duration_outlier", &front, 0, PlayerClockSource::None, 0,
                 front.durationUs);
      }
      if (prepared.hadMassiveGlitch) {
        logVideo("pts_repair_glitch", &prepared.frame, prepared.frame.ptsUs,
                 PlayerClockSource::None, prepared.ptsRepairErrorUs,
                 prepared.frame.durationUs);
        playback_clock::synchronizeAudioClockOnly(static_cast<int>(serial),
                                                  prepared.frame.ptsUs);
      }

      playback_sync::FramePlan plan = playback_sync::planFrame(
          syncState, st, prepared, master, videoClock, now);
      if (plan.syncLost) {
        logVideo("sync_lost", &prepared.frame, masterUs, master.source,
                 prepared.frame.ptsUs - masterUs, plan.delayUs);
      }
      if (st == PlayerState::Priming || plan.syncLost) {
        if (plan.syncLost) {
          logVideo("sync_lost_freerun", &prepared.frame, masterUs, master.source,
                   prepared.frame.ptsUs - masterUs, plan.delayUs);
        } else {
          logVideo("priming_present", &prepared.frame, masterUs, master.source,
                   0, plan.delayUs);
        }
      }

      lastMasterUs.store(masterUs, std::memory_order_relaxed);
      lastMasterSource.store(static_cast<int>(master.source),
                             std::memory_order_relaxed);
      lastDiffUs.store(plan.diffUs, std::memory_order_relaxed);
      lastDelayUs.store(plan.delayUs, std::memory_order_relaxed);

      if (plan.waitForAudioClock) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        syncState.lastPresentedTimeUs = nowUs();
        continue;
      }
      if (plan.waitForAudioRecovery) {
        logVideo("audio_starved", &prepared.frame, masterUs, master.source,
                 plan.diffUs, plan.delayUs);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        syncState.lastPresentedTimeUs = nowUs();
        continue;
      }
      if (plan.dropForBehind) {
        size_t qDepth = videoFrames.size();
        if (qDepth > 1) {
          logVideo("skip_behind", &prepared.frame, masterUs, master.source,
                   plan.diffUs, plan.delayUs);
          QueuedFrame drop{};
          if (videoFrames.pop(&drop)) {
            videoFrames.release(drop.poolIndex);
          }
          continue;
        }
        plan.delayUs = 0;
        plan.targetUs = syncState.frameTimerUs;
      }

      int64_t targetUs = plan.targetUs;
      if (now < targetUs) {
        int64_t sleepUs = targetUs - now;
        if (sleepUs > 0) {
          if (master.source == PlayerClockSource::Audio && sleepUs > 1000) {
              uint64_t lastCounter = audioStreamUpdateCounter();
              audioStreamWaitForUpdate(lastCounter, (int)(sleepUs / 1000));
          } else {
              // Limit sleep to 10ms to keep the UI/commands responsive.
              int64_t actualSleep = (std::min)(sleepUs, static_cast<int64_t>(10000));
              std::this_thread::sleep_for(std::chrono::microseconds(actualSleep));
          }
        }
        continue;
      }
      if (playback_sync::shouldDropLateFrame(now, targetUs, videoFrames.size())) {
        {
          logVideo("drop_late", &prepared.frame, masterUs, master.source,
                   plan.diffUs, plan.delayUs);
          QueuedFrame drop{};
          if (videoFrames.pop(&drop)) {
            videoFrames.release(drop.poolIndex);
            playback_sync::noteLateDrop(syncState, prepared, targetUs, now);
          }
          std::this_thread::sleep_for(std::chrono::microseconds(100));
          continue;
        }
      }

      QueuedFrame item{};
      if (!videoFrames.pop(&item)) {
        continue;
      }
      item.ptsUs = prepared.frame.ptsUs;
      VideoFrame local = videoFrames.frame(item.poolIndex);
      videoFrames.release(item.poolIndex);
      {
        std::lock_guard<std::mutex> lock(currentFrameMutex);
        currentFrame = std::move(local);
        hasFrame.store(true, std::memory_order_relaxed);
      }
      frameCounter.fetch_add(1, std::memory_order_relaxed);
      if (frameReadyEvent) {
        SetEvent(frameReadyEvent);
      }
      frameCv.notify_all();
      
      // Ensure we yield to other threads (decoder, audio, etc.)
      std::this_thread::yield();
      
      lastPresentedPtsUs.store(item.ptsUs, std::memory_order_relaxed);
      lastPresentedDurationUs.store(plan.delayUs, std::memory_order_relaxed);
      lastPresentedSerial.store(static_cast<int>(serial),
                                std::memory_order_relaxed);
      {
        std::lock_guard<std::mutex> lock(lastInfoMutex);
        lastInfo = item.info;
        lastInfoValid.store(true, std::memory_order_relaxed);
      }
      int64_t presentedNowUs = nowUs();
      videoClock.set(item.ptsUs, presentedNowUs, static_cast<int>(serial));
      serialControl.clearPendingPresentation(static_cast<int>(serial));
      logVideo("present", &item, masterUs, master.source, plan.diffUs,
               plan.delayUs);
      
      // Monitor queue depth for resource bottlenecks
      size_t qDepth = videoFrames.size();
      if (qDepth <= 2) {
        // Queue critically low - potential decode stall
        logVideo("queue_low", &item, masterUs, master.source, plan.diffUs,
                 plan.delayUs);
      } else if (qDepth >= 30) {
        // Queue critically high - potential render bottleneck
        logVideo("queue_high", &item, masterUs, master.source, plan.diffUs,
                 plan.delayUs);
      }

      if (!syncState.firstPresentedForSerial) {
        syncState.firstPresentedForSerial = true;
        PlayerEvent ev{};
        ev.type = PlayerEventType::FirstFramePresented;
        ev.serial = static_cast<int>(serial);
        postEvent(ev);
      }
      playback_sync::notePresentedFrame(syncState, prepared, plan.delayUs,
                                        targetUs, presentedNowUs);
      if (plan.delayUs > 0) {
        double frameDurSec = static_cast<double>(plan.delayUs) / 1000000.0;
        size_t needed =
            static_cast<size_t>(std::ceil(kVideoBufferHighSec / frameDurSec)) +
            2;
        if (needed < 3) needed = 3;
        constexpr size_t kMinQueuedFrames = 6;
        if (needed < kMinQueuedFrames) needed = kMinQueuedFrames;
        if (needed > 16) needed = 16;
        maxQueue.store(needed, std::memory_order_relaxed);
      }
    }
  }

  ~Impl() {
    if (frameReadyEvent) {
      CloseHandle(frameReadyEvent);
      frameReadyEvent = nullptr;
    }
  }
};

Player::Player() : impl_(std::make_unique<Impl>()) {}

Player::~Player() { close(); }

bool Player::open(const PlayerConfig& config, std::string* error) {
  if (!impl_) return false;
  (void)error;
  if (impl_->ctrlRunning.load()) {
    close();
  }
  impl_->config = config;
  impl_->logPath = config.logPath;
  impl_->ctrlRunning.store(true, std::memory_order_relaxed);
  impl_->controlThread = std::thread([this]() { impl_->controlMain(); });
  return true;
}

void Player::close() {
  if (!impl_) return;
  impl_->appendTimingFmt("player_close begin ctrl_running=%d control_joinable=%d",
                         impl_->ctrlRunning.load(std::memory_order_relaxed) ? 1
                                                                             : 0,
                         impl_->controlThread.joinable() ? 1 : 0);
  if (impl_->controlThread.joinable()) {
    if (impl_->ctrlRunning.load()) {
      impl_->appendTimingFmt("player_close post_close_request");
      impl_->postEvent(PlayerEvent{PlayerEventType::CloseRequest});
    } else {
      impl_->appendTimingFmt("player_close notify_control_thread");
      impl_->eventCv.notify_one();
    }
    impl_->ctrlRunning.store(false, std::memory_order_relaxed);
    impl_->appendTimingFmt("player_close control_join_begin");
    impl_->controlThread.join();
    impl_->appendTimingFmt("player_close control_join_end");
  } else {
    impl_->appendTimingFmt("player_close stop_threads_without_control_begin");
    impl_->stopThreads();
    impl_->appendTimingFmt("player_close stop_threads_without_control_end");
  }
  impl_->appendTimingFmt("player_close end");
}

void Player::requestSeek(int64_t targetUs) {
  if (!impl_ || !impl_->ctrlRunning.load()) return;
  PlayerEvent ev{};
  ev.type = PlayerEventType::SeekRequest;
  ev.arg1 = targetUs;
  impl_->postEvent(ev);
}

void Player::requestResize(int targetW, int targetH) {
  if (!impl_ || !impl_->ctrlRunning.load()) return;
  PlayerEvent ev{};
  ev.type = PlayerEventType::ResizeRequest;
  ev.arg1 = targetW;
  ev.arg2 = targetH;
  impl_->postEvent(ev);
}

void Player::setVideoPaused(bool paused) {
  if (!impl_) return;
  PlayerEvent ev{};
  ev.type = PlayerEventType::PauseRequest;
  ev.arg1 = paused ? 1 : 0;
  impl_->postEvent(ev);
}

size_t Player::audioTrackCount() const {
  if (!impl_) return 0;
  std::lock_guard<std::mutex> lock(impl_->audioTrackMutex);
  return impl_->audioTrackStreams.size();
}

std::string Player::activeAudioTrackLabel() const {
  if (!impl_) return "N/A";
  std::lock_guard<std::mutex> lock(impl_->audioTrackMutex);
  int active = impl_->activeAudioTrack.load(std::memory_order_relaxed);
  if (active < 0 ||
      active >= static_cast<int>(impl_->audioTrackLabels.size())) {
    return "N/A";
  }
  const std::string& label = impl_->audioTrackLabels[static_cast<size_t>(active)];
  return label.empty() ? "N/A" : label;
}

bool Player::canCycleAudioTracks() const { return audioTrackCount() > 1; }

bool Player::cycleAudioTrack() {
  if (!impl_ || !impl_->ctrlRunning.load(std::memory_order_relaxed)) return false;
  {
    std::lock_guard<std::mutex> lock(impl_->audioTrackMutex);
    if (impl_->audioTrackStreams.size() <= 1) return false;
  }
  PlayerEvent ev{};
  ev.type = PlayerEventType::CycleAudioTrack;
  impl_->postEvent(ev);
  return true;
}

bool Player::initDone() const {
  return impl_ && impl_->initDone.load();
}

bool Player::initOk() const {
  return impl_ && impl_->initOk.load();
}

std::string Player::initError() const {
  if (!impl_) return std::string();
  std::lock_guard<std::mutex> lock(impl_->initMutex);
  return impl_->initError;
}

bool Player::audioStarting() const {
  return impl_ && !impl_->audioStartDone.load();
}

bool Player::audioOk() const {
  return impl_ && impl_->audioStartOk.load();
}

bool Player::audioFinished() const {
  if (!impl_) return false;
  return impl_->audioStartOk.load() && audioIsFinished();
}

bool Player::isSeeking() const {
  if (!impl_) return false;
  return impl_->state.load() == PlayerState::Seeking;
}

bool Player::isBuffering() const {
  if (!impl_) return false;
  PlayerState st = impl_->state.load();
  return st == PlayerState::Prefill || st == PlayerState::Priming ||
         st == PlayerState::Seeking || st == PlayerState::Opening;
}

bool Player::isEnded() const {
  if (!impl_) return false;
  return impl_->state.load() == PlayerState::Ended;
}

int64_t Player::durationUs() const {
  if (!impl_) return 0;
  int64_t dur = impl_->durationUs.load();
  if (dur <= 0) return 0;
  return dur;
}

int64_t Player::currentUs() const {
  if (!impl_) return 0;
  int64_t seekUs = impl_->serialControl.seekDisplayUs();
  if (impl_->serialControl.pendingSeekSerial() != 0) {
    return seekUs;
  }
  int64_t now = nowUs();
  playback_clock::Snapshot snapshot = playback_clock::sample(
      impl_->audioStartOk.load(std::memory_order_relaxed) && !audioIsFinished(),
      impl_->serialControl.currentSerial(), impl_->videoClock, now);
  return playback_clock::resolveCurrentPlaybackUs(
      snapshot, impl_->videoClock, impl_->serialControl.currentSerial(),
      impl_->lastPresentedPtsUs.load(std::memory_order_relaxed), now);
}

uint64_t Player::videoFrameCounter() const {
  if (!impl_) return 0;
  return impl_->frameCounter.load(std::memory_order_relaxed);
}

bool Player::waitForVideoFrame(uint64_t lastCounter, int timeoutMs) const {
  if (!impl_) return false;
  if (impl_->frameCounter.load(std::memory_order_relaxed) != lastCounter) {
    return true;
  }
  if (timeoutMs <= 0) {
    return false;
  }
  if (impl_->frameReadyEvent) {
    WaitForSingleObject(impl_->frameReadyEvent,
                        static_cast<DWORD>(std::max(0, timeoutMs)));
    return impl_->frameCounter.load(std::memory_order_relaxed) != lastCounter;
  }
  std::unique_lock<std::mutex> lock(impl_->frameCvMutex);
  impl_->frameCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
    return impl_->frameCounter.load(std::memory_order_relaxed) != lastCounter ||
           !impl_->running.load(std::memory_order_relaxed);
  });
  return impl_->frameCounter.load(std::memory_order_relaxed) != lastCounter;
}

HANDLE Player::videoFrameWaitHandle() const {
  if (!impl_) return nullptr;
  return impl_->frameReadyEvent;
}

bool Player::tryGetVideoFrame(VideoFrame* out) {
  if (!impl_ || !out) return false;
  if (!impl_->hasFrame.load(std::memory_order_relaxed)) {
    return false;
  }
  uint64_t counter = impl_->frameCounter.load(std::memory_order_relaxed);
  uint64_t last = impl_->lastReadCounter.load(std::memory_order_relaxed);
  if (counter == last) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->currentFrameMutex);
  if (!impl_->hasFrame.load(std::memory_order_relaxed)) {
    return false;
  }
  *out = impl_->currentFrame;
  impl_->lastReadCounter.store(counter, std::memory_order_relaxed);
  return true;
}

bool Player::copyCurrentVideoFrame(VideoFrame* out) {
  if (!impl_ || !out) return false;
  if (!impl_->hasFrame.load(std::memory_order_relaxed)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->currentFrameMutex);
  if (!impl_->hasFrame.load(std::memory_order_relaxed)) {
    return false;
  }
  *out = impl_->currentFrame;
  return true;
}

bool Player::hasVideoFrame() const {
  return impl_ && impl_->hasFrame.load(std::memory_order_relaxed);
}

PlayerState Player::state() const {
  if (!impl_) return PlayerState::Idle;
  return impl_->state.load(std::memory_order_relaxed);
}

PlayerDebugInfo Player::debugInfo() const {
  PlayerDebugInfo info{};
  if (!impl_) return info;
  info.state = impl_->state.load(std::memory_order_relaxed);
  info.currentSerial = impl_->serialControl.currentSerial();
  info.pendingSeekSerial = impl_->serialControl.pendingSeekSerial();
  info.seekInFlightSerial = impl_->serialControl.seekInFlightSerial();
  info.videoQueueDepth = impl_->videoFrames.size();
  info.lastPresentedPtsUs =
      impl_->lastPresentedPtsUs.load(std::memory_order_relaxed);
  info.lastPresentedDurationUs =
      impl_->lastPresentedDurationUs.load(std::memory_order_relaxed);
  info.lastDiffUs = impl_->lastDiffUs.load(std::memory_order_relaxed);
  info.lastDelayUs = impl_->lastDelayUs.load(std::memory_order_relaxed);
  info.masterClockUs = impl_->lastMasterUs.load(std::memory_order_relaxed);
  info.masterSource =
      static_cast<PlayerClockSource>(impl_->lastMasterSource.load(
          std::memory_order_relaxed));
  info.audioOk = impl_->audioStartOk.load(std::memory_order_relaxed);
  int64_t now = nowUs();
  playback_clock::Snapshot snapshot = playback_clock::sample(
      impl_->audioStartOk.load(std::memory_order_relaxed) && !audioIsFinished(),
      impl_->serialControl.currentSerial(), impl_->videoClock, now);
  info.audioStarved = snapshot.audioStarved;
  info.audioClockReady = snapshot.audioClockReady;
  info.audioClockFresh = snapshot.audioClockFresh;
  info.audioClockUs = snapshot.source == PlayerClockSource::Audio ? snapshot.us : 0;
  info.audioClockUpdatedUs = snapshot.audioClockUpdatedUs;
  info.audioBufferedFrames = snapshot.audioBufferedFrames;
  info.audioSampleRate = snapshot.audioSampleRate;
  return info;
}

int Player::sourceWidth() const {
  if (!impl_) return 0;
  return impl_->sourceWidth.load(std::memory_order_relaxed);
}

int Player::sourceHeight() const {
  if (!impl_) return 0;
  return impl_->sourceHeight.load(std::memory_order_relaxed);
}
