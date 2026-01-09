#include "videoplayback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <new>
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
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/frame.h>
#include <libavutil/rational.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/mem.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include "asciiart.h"
#include "asciiart_gpu.h"
#include "audioplayback.h"
#include "clock.h"
#include "ui_helpers.h"
#include "videodecoder.h"

#include "timing_log.h"

namespace {
#if RADIOIFY_ENABLE_FFMPEG_ERROR_LOG
std::mutex gFfmpegLogMutex;
std::filesystem::path gFfmpegLogPath;

void ffmpegLogCallback(void* ptr, int level, const char* fmt, va_list vl) {
  if (level > AV_LOG_ERROR) return;
  char line[1024];
  int printPrefix = 1;
  av_log_format_line2(ptr, level, fmt, vl, line, sizeof(line), &printPrefix);
  std::filesystem::path path;
  {
    std::lock_guard<std::mutex> lock(gFfmpegLogMutex);
    path = gFfmpegLogPath;
  }
  if (path.empty()) return;
  std::ofstream f(path, std::ios::app);
  if (!f) return;
  f << radioifyLogTimestamp() << " ffmpeg_error " << line;
  size_t len = std::strlen(line);
  if (len == 0 || line[len - 1] != '\n') {
    f << "\n";
  }
}
#endif

constexpr int64_t kProbeSize = 64 * 1024;
constexpr int64_t kAnalyzeDurationFastUs = 500000;
constexpr int64_t kAnalyzeDurationFallbackUs = 5000000;
constexpr size_t kIoCacheSize = 32 * 1024 * 1024;
constexpr size_t kIoCacheLowWater = 8 * 1024 * 1024;
constexpr size_t kIoCacheHighWater = 24 * 1024 * 1024;
constexpr int kIoAvioBufferSize = 64 * 1024;

std::string ffmpegError(int err) {
  char buf[256];
  buf[0] = '\0';
  av_strerror(err, buf, sizeof(buf));
  return std::string(buf);
}

int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
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

static enum AVPixelFormat get_hw_format(AVCodecContext* ctx,
                                        const enum AVPixelFormat* pix_fmts) {
  const enum AVPixelFormat* p;
  for (p = pix_fmts; *p != -1; p++) {
    if (*p == AV_PIX_FMT_D3D11) return *p;
  }
  return avcodec_default_get_format(ctx, pix_fmts);
}

int64_t rescaleToFrames(int64_t value, AVRational src, uint32_t sampleRate) {
  if (value <= 0 || sampleRate == 0) return 0;
  AVRational dst{1, static_cast<int>(sampleRate)};
  return av_rescale_q(value, src, dst);
}

struct PerfLog {
  std::ofstream file;
  std::string buffer;
  bool enabled = false;
};

GpuAsciiRenderer& sharedGpuRenderer() {
  static GpuAsciiRenderer renderer;
  return renderer;
}

// C-style callbacks for FFmpeg D3D11 locking.
static void d3d11_lock(void* ctx) {
  if (ctx) reinterpret_cast<std::recursive_mutex*>(ctx)->lock();
}

static void d3d11_unlock(void* ctx) {
  if (ctx) reinterpret_cast<std::recursive_mutex*>(ctx)->unlock();
}

// Mutex to synchronize access to the shared D3D11 immediate context.
// D3D11 immediate context is NOT thread-safe, and we use it from both
// the rendering thread (main) and the decoding thread (via FFmpeg).
std::recursive_mutex& getSharedGpuMutex() {
    static std::recursive_mutex mtx;
    return mtx;
}

// Returns the shared D3D11 device for zero-copy video decoding
// This ensures the same device is used by both decoder and renderer
ID3D11Device* getSharedGpuDevice() {
  static bool initialized = false;
  static std::mutex initMutex;
  
  GpuAsciiRenderer& renderer = sharedGpuRenderer();
  
  // Lazy init the renderer to get the device
  if (!initialized) {
    std::lock_guard<std::mutex> lock(initMutex);
    if (!initialized) {
      std::string error;
      // Initialize with reasonable defaults - will be resized on first render
      if (renderer.Initialize(1920, 1080, &error)) {
        initialized = true;
      }
    }
  }
  
  return renderer.device();
}

bool perfLogOpen(PerfLog* log, const std::filesystem::path& path,
                 std::string* error) {
#if RADIOIFY_ENABLE_TIMING_LOG
  if (!log) return false;
  log->file.open(path, std::ios::out | std::ios::app);
  if (!log->file.is_open()) {
    if (error) {
      *error = "Failed to open timing log file: " + toUtf8String(path);
    }
    return false;
  }
  log->enabled = true;
  return true;
#else
  (void)path;
  (void)error;
  if (log) log->enabled = false;
  return true;
#endif
}

void perfLogFlush(PerfLog* log) {
  if (!log || !log->enabled || log->buffer.empty()) return;
  log->file << log->buffer;
  log->file.flush();
  log->buffer.clear();
}

void perfLogAppendf(PerfLog* log, const char* fmt, ...) {
  if (!log || !log->enabled) return;
  char buf[2048];
  va_list args;
  va_start(args, fmt);
  int written = std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (written <= 0) return;
  if (written >= static_cast<int>(sizeof(buf))) {
    written = static_cast<int>(sizeof(buf)) - 1;
  }
  std::string ts = radioifyLogTimestamp();
  log->buffer.append(ts);
  log->buffer.push_back(' ');
  log->buffer.append(buf, buf + written);
  log->buffer.push_back('\n');
  if (log->buffer.size() >= 8192) perfLogFlush(log);
}

void perfLogClose(PerfLog* log) {
  if (!log) return;
  perfLogFlush(log);
  if (log->file.is_open()) {
    log->file.close();
  }
  log->enabled = false;
}

struct QueuedPacket {
  AVPacket pkt{};
  uint64_t serial = 0;
  bool flush = false;
  bool eof = false;
};

struct PacketQueue {
  mutable std::mutex mutex;
  std::condition_variable cv;
  std::deque<QueuedPacket> packets;
  size_t bytes = 0;
  size_t maxBytes = 0;
  bool aborted = false;

  void init(size_t maxBytesIn) {
    maxBytes = maxBytesIn;
    aborted = false;
    bytes = 0;
    packets.clear();
  }

  void abortQueue() {
    std::lock_guard<std::mutex> lock(mutex);
    aborted = true;
    cv.notify_all();
  }

  void flush() {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& item : packets) {
      av_packet_unref(&item.pkt);
    }
    packets.clear();
    bytes = 0;
    cv.notify_all();
  }

  void flush(uint64_t serial) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& item : packets) {
      av_packet_unref(&item.pkt);
    }
    packets.clear();
    bytes = 0;
    QueuedPacket item;
    av_init_packet(&item.pkt);
    item.serial = serial;
    item.flush = true;
    item.eof = false;
    packets.push_back(std::move(item));
    cv.notify_all();
  }

  bool pushPacket(const AVPacket* pkt, uint64_t serial) {
    if (!pkt) return false;
    size_t packetBytes =
        static_cast<size_t>(std::max(0, pkt->size));
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&]() {
      return aborted || packets.empty() || bytes + packetBytes <= maxBytes;
    });
    if (aborted) return false;
    QueuedPacket item;
    av_init_packet(&item.pkt);
    if (av_packet_ref(&item.pkt, pkt) < 0) {
      return false;
    }
    item.serial = serial;
    item.flush = false;
    item.eof = false;
    packets.push_back(std::move(item));
    bytes += packetBytes;
    cv.notify_all();
    return true;
  }

  bool pushFlush(uint64_t serial) {
    std::unique_lock<std::mutex> lock(mutex);
    if (aborted) return false;
    QueuedPacket item;
    av_init_packet(&item.pkt);
    item.serial = serial;
    item.flush = true;
    item.eof = false;
    packets.push_back(std::move(item));
    cv.notify_all();
    return true;
  }

  bool pushEof(uint64_t serial) {
    std::unique_lock<std::mutex> lock(mutex);
    if (aborted) return false;
    QueuedPacket item;
    av_init_packet(&item.pkt);
    item.serial = serial;
    item.flush = false;
    item.eof = true;
    packets.push_back(std::move(item));
    cv.notify_all();
    return true;
  }

  bool pop(QueuedPacket* out) {
    if (!out) return false;
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&]() { return aborted || !packets.empty(); });
    if (aborted) return false;
    *out = std::move(packets.front());
    packets.pop_front();
    if (!out->flush && !out->eof) {
      bytes -= static_cast<size_t>(std::max(0, out->pkt.size));
    }
    cv.notify_all();
    return true;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex);
    return packets.size();
  }
};

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
    file = std::fopen(path.string().c_str(), "rb");
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
    buffer.clear();
    capacity = 0;
    size = 0;
  }

  int read(uint8_t* dst, int dstSize) {
    if (!dst || dstSize <= 0) return 0;
    std::unique_lock<std::mutex> lock(mutex);
    while (!aborted && size == 0 && !eof) {
      if ((running && !running->load()) ||
          (commandPending && commandPending->load())) {
        return AVERROR_EXIT;
      }
      cv.wait_for(lock, std::chrono::milliseconds(10));
    }
    if (aborted) return AVERROR_EXIT;
    if (size == 0 && eof) {
      return 0;
    }
    if (commandPending && commandPending->load()) {
      return AVERROR_EXIT;
    }
    size_t toCopy = std::min<size_t>(static_cast<size_t>(dstSize), size);
    size_t first = std::min(toCopy, capacity - readPos);
    std::memcpy(dst, buffer.data() + readPos, first);
    if (toCopy > first) {
      std::memcpy(dst + first, buffer.data(), toCopy - first);
    }
    readPos = (readPos + toCopy) % capacity;
    size -= toCopy;
    cv.notify_all();
    return static_cast<int>(toCopy);
  }

  int64_t seek(int64_t offset, int whence) {
    if (whence == AVSEEK_SIZE) {
      return fileSize;
    }
    int whenceBase = whence & ~AVSEEK_FORCE;
    {
      std::lock_guard<std::mutex> lock(mutex);
      seekPending = true;
      seekOffset = offset;
      seekWhence = whenceBase;
      seekDone = false;
      eof = false;
      cv.notify_all();
    }
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&]() { return aborted || seekDone; });
    if (aborted) return AVERROR_EXIT;
    return filePos;
  }

  IoCacheStats stats() const {
    std::lock_guard<std::mutex> lock(mutex);
    IoCacheStats stats;
    stats.size = size;
    stats.capacity = capacity;
    stats.lowWater = lowWater;
    stats.highWater = highWater;
    stats.eof = eof;
    stats.seekPending = seekPending;
    stats.filePos = filePos;
    stats.fileSize = fileSize;
    return stats;
  }

 private:
  int64_t logicalPosLocked() const {
    int64_t pos = filePos - static_cast<int64_t>(size);
    return std::max<int64_t>(0, pos);
  }

  void performSeekLocked() {
    int64_t base = 0;
    if (seekWhence == SEEK_CUR) {
      base = logicalPosLocked();
    } else if (seekWhence == SEEK_END) {
      base = (fileSize >= 0) ? fileSize : 0;
    }
    int64_t target = base + seekOffset;
    if (fileSize >= 0) {
      target = std::clamp<int64_t>(target, 0, fileSize);
    } else if (target < 0) {
      target = 0;
    }
    std::clearerr(file);
#if defined(_WIN32)
    if (_fseeki64(file, target, SEEK_SET) == 0) {
#else
    if (std::fseek(file, static_cast<long>(target), SEEK_SET) == 0) {
#endif
      filePos = target;
      readPos = 0;
      writePos = 0;
      size = 0;
      eof = false;
    } else {
      eof = true;
    }
    seekPending = false;
    seekDone = true;
    cv.notify_all();
  }

  void threadMain() {
    bool filling = false;
    while (true) {
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait(lock, [&]() {
        return aborted || seekPending || (!eof && (filling || size < lowWater));
      });
      if (aborted) break;
      if (seekPending) {
        performSeekLocked();
        filling = false;
        continue;
      }
      if (eof) {
        continue;
      }
      if (!filling && size < lowWater) {
        filling = true;
      }
      if (!filling) {
        continue;
      }
      size_t freeSpace = capacity - size;
      if (freeSpace == 0) {
        filling = false;
        continue;
      }
      size_t contiguous = std::min(freeSpace, capacity - writePos);
      lock.unlock();
      size_t readCount =
          std::fread(buffer.data() + writePos, 1, contiguous, file);
      lock.lock();
      if (readCount == 0) {
        if (std::feof(file)) {
          eof = true;
        } else if (std::ferror(file)) {
          eof = true;
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

struct DemuxContext {
  AVFormatContext* fmt = nullptr;
  AVIOContext* avio = nullptr;
  std::unique_ptr<IoCache> io;
  int videoStreamIndex = -1;
  int audioStreamIndex = -1;
  AVRational videoTimeBase{0, 1};
  AVRational audioTimeBase{0, 1};
  int64_t formatStartUs = 0;
  int64_t durationUs = 0;
};

struct DemuxInterrupt {
  std::atomic<bool>* running = nullptr;
  std::atomic<bool>* commandPending = nullptr;
};

int demuxInterruptCallback(void* opaque) {
  auto* interrupt = reinterpret_cast<DemuxInterrupt*>(opaque);
  if (!interrupt) return 0;
  if (interrupt->running && !interrupt->running->load()) return 1;
  if (interrupt->commandPending && interrupt->commandPending->load()) return 1;
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

  int openErr = avformat_open_input(&fmt,
                                    ioCache ? nullptr : path.string().c_str(),
                                    nullptr, &options);
  av_dict_free(&options);
  if (openErr < 0) {
    std::string msg = "Failed to open media: " + ffmpegError(openErr);
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
  fmt->flags |= AVFMT_FLAG_NOBUFFER;
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
    fmt->flags |= AVFMT_FLAG_NOBUFFER;
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
    out->videoTimeBase = fmt->streams[out->videoStreamIndex]->time_base;
  }
  const AVCodec* acodec = nullptr;
  out->audioStreamIndex =
      av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &acodec, 0);
  if (out->audioStreamIndex >= 0) {
    out->audioTimeBase = fmt->streams[out->audioStreamIndex]->time_base;
  }

  if (out->videoStreamIndex < 0 && out->audioStreamIndex < 0) {
    avformat_close_input(&fmt);
    if (error) *error = "No playable streams found.";
    return false;
  }
  return true;
}

bool initVideoDecoder(const DemuxContext& demux,
                      bool preferHardware,
                      VideoDecodeContext* out,
                      std::string* error) {
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
    if (sharedDevice) {
      AVBufferRef* sharedCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
      if (sharedCtx) {
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
        if (av_hwdevice_ctx_init(sharedCtx) >= 0) {
          hwDeviceCtx = sharedCtx;
          usingSharedDevice = true;
        } else {
          av_buffer_unref(&sharedCtx);
        }
      }
    }
    if (!hwDeviceCtx) {
      if (av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_D3D11VA,
                                 nullptr, nullptr, 0) >= 0) {
        usingSharedDevice = false;
      }
    }
    if (hwDeviceCtx) {
      ctx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
      ctx->get_format = get_hw_format;
      ctx->extra_hw_frames = 32;
      if (usingSharedDevice) {
        configureD3d11Frames(ctx, ctx->hw_device_ctx, true);
      }
    }
  }

  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    if (hwDeviceCtx) av_buffer_unref(&hwDeviceCtx);
    avcodec_free_context(&ctx);
    if (error) *error = "Failed to open video decoder.";
    return false;
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
  out->width = ctx->width;
  out->height = ctx->height;
  out->targetW = out->width;
  out->targetH = out->height;
  if (!usingSharedDevice) {
    clampTargetSize(out->targetW, out->targetH);
  }
  out->fullRange = mapFullRange(ctx->color_range);
  out->yuvMatrix = mapColorMatrix(ctx->colorspace);
  out->yuvTransfer = mapColorTransfer(ctx->color_trc);
  out->formatStartUs = demux.formatStartUs;
  out->useSharedDevice = usingSharedDevice;
  return true;
}

bool initAudioDecoder(const DemuxContext& demux,
                      uint32_t outRate,
                      uint32_t outChannels,
                      AudioDecodeContext* out,
                      std::string* error) {
  if (!out || demux.audioStreamIndex < 0) return false;
  AVStream* stream = demux.fmt->streams[demux.audioStreamIndex];
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
  ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
  ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
  ctx->err_recognition = AV_EF_IGNORE_ERR;
  if (ctx->thread_count < 0) {
    ctx->thread_count = 0;
  }
  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    avcodec_free_context(&ctx);
    if (error) *error = "Failed to open audio decoder.";
    return false;
  }

  SwrContext* swr = nullptr;
  AVChannelLayout inLayout{};
  int inChannels = ctx->ch_layout.nb_channels;
  if (inChannels <= 0) {
    inChannels = 1;
  }
  if (ctx->ch_layout.nb_channels > 0) {
    av_channel_layout_copy(&inLayout, &ctx->ch_layout);
  } else {
    av_channel_layout_default(&inLayout, inChannels);
  }
  AVChannelLayout outLayout{};
  av_channel_layout_default(&outLayout, static_cast<int>(outChannels));

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

bool emitVideoFrame(VideoDecodeContext* ctx,
                    VideoFrame& out,
                    VideoReadInfo* info,
                    AVFrame* src,
                    bool decodePixels,
                    bool keepOnGpu) {
  if (!ctx || !src) return false;
  int64_t ts100ns = frameTimestamp100ns(*ctx, src);
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

  int outW = keepOnGpu ? src->width : ctx->targetW;
  int outH = keepOnGpu ? src->height : ctx->targetH;
  out.width = outW;
  out.height = outH;
  out.timestamp100ns = ts100ns;
  out.fullRange = ctx->fullRange;
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
      info->duration100ns = 0;
    }
    return true;
  }

  if (keepOnGpu && src->format == AV_PIX_FMT_D3D11) {
    auto* texture = reinterpret_cast<ID3D11Texture2D*>(src->data[0]);
    if (!texture) return false;
    intptr_t arrayIndex = reinterpret_cast<intptr_t>(src->data[1]);
    out.format = VideoPixelFormat::HWTexture;
    out.hwTexture = texture;
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

  uint8_t* yPlane = out.yuv.data();
  uint8_t* uvPlane =
      yPlane + static_cast<size_t>(stride) * static_cast<size_t>(planeHeight);
  uint8_t* dstData[4] = {yPlane, uvPlane, nullptr, nullptr};
  int dstLinesize[4] = {stride, stride, 0, 0};

  AVPixelFormat srcFmt = static_cast<AVPixelFormat>(cpuSrc->format);
  if (srcFmt == AV_PIX_FMT_NV12 && cpuSrc->width == dstW &&
      cpuSrc->height == dstH) {
    for (int y = 0; y < dstH; ++y) {
      std::memcpy(yPlane + y * stride,
                  cpuSrc->data[0] + y * cpuSrc->linesize[0],
                  static_cast<size_t>(dstW));
    }
    int uvH = dstH / 2;
    for (int y = 0; y < uvH; ++y) {
      std::memcpy(uvPlane + y * stride,
                  cpuSrc->data[1] + y * cpuSrc->linesize[1],
                  static_cast<size_t>(dstW));
    }
  } else {
    int flags = SWS_FAST_BILINEAR;
    if (static_cast<int64_t>(cpuSrc->width) * cpuSrc->height >
        2560 * 1440) {
      flags = SWS_POINT;
    }
    ctx->sws = sws_getCachedContext(
        ctx->sws, cpuSrc->width, cpuSrc->height, srcFmt, dstW, dstH,
        AV_PIX_FMT_NV12, flags, nullptr, nullptr, nullptr);
    if (!ctx->sws) {
      return false;
    }
    sws_scale(ctx->sws, cpuSrc->data, cpuSrc->linesize, 0, cpuSrc->height,
              dstData, dstLinesize);
  }

  if (info) {
    info->timestamp100ns = ts100ns;
    if (cpuSrc->duration > 0) {
      info->duration100ns = static_cast<int64_t>(
          av_rescale_q(cpuSrc->duration, ctx->timeBase,
                       AVRational{1, 10000000}));
    } else {
      info->duration100ns = 0;
    }
  }
  return true;
}

void finalizeVideoPlayback(ConsoleScreen& screen, bool fullRedrawEnabled,
                           PerfLog* log) {
  if (fullRedrawEnabled) {
    screen.setAlwaysFullRedraw(false);
  }
  perfLogClose(log);
}
}  // namespace

void configureFfmpegVideoLog(const std::filesystem::path& path) {
#if RADIOIFY_ENABLE_FFMPEG_ERROR_LOG
  {
    std::lock_guard<std::mutex> lock(gFfmpegLogMutex);
    gFfmpegLogPath = path;
  }
  av_log_set_level(AV_LOG_ERROR);
  av_log_set_callback(ffmpegLogCallback);
#else
  (void)path;
  av_log_set_level(AV_LOG_QUIET);
#endif
}

bool showAsciiVideo(const std::filesystem::path& file, ConsoleInput& input,
                    ConsoleScreen& screen, const Style& baseStyle,
                    const Style& accentStyle, const Style& dimStyle,
                    const Style& progressEmptyStyle,
                    const Style& progressFrameStyle,
                    const Color& progressStart, const Color& progressEnd,
                    const VideoPlaybackConfig& config) {
  bool enableAscii = config.enableAscii;
  bool enableAudio = config.enableAudio && audioIsEnabled();

  bool fullRedrawEnabled = enableAscii;
  if (fullRedrawEnabled) {
    screen.setAlwaysFullRedraw(true);
  }

  auto showError = [&](const std::string& message,
                       const std::string& detail) -> bool {
    InputEvent ev{};
    while (true) {
      screen.updateSize();
      int width = std::max(20, screen.width());
      screen.clear(baseStyle);
      std::string title = "Video: " + toUtf8String(file.filename());
      screen.writeText(0, 0, fitLine(title, width), accentStyle);
      screen.writeText(0, 1, fitLine("Press any key to return", width),
                       dimStyle);
      std::string line = message;
      std::string extra = detail;
      if (!extra.empty() && extra == line) {
        extra.clear();
      }
      if (line.empty() && !extra.empty()) {
        line = extra;
        extra.clear();
      }
      if (line.empty()) {
        line = "Failed to open video.";
      }
      screen.writeText(0, 3, fitLine(line, width), dimStyle);
      if (!extra.empty()) {
        screen.writeText(0, 4, fitLine(extra, width), dimStyle);
      }
      screen.draw();
      while (input.poll(ev)) {
        if (ev.type == InputEvent::Type::Key) {
          return true;
        }
        if (ev.type == InputEvent::Type::Resize) {
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  };

  auto showAudioFallbackPrompt = [&](const std::string& message,
                                     const std::string& detail) -> bool {
    InputEvent ev{};
    while (true) {
      screen.updateSize();
      int width = std::max(20, screen.width());
      screen.clear(baseStyle);
      std::string title = "Video: " + toUtf8String(file.filename());
      screen.writeText(0, 0, fitLine(title, width), accentStyle);
      screen.writeText(0, 2, fitLine(message, width), dimStyle);
      if (!detail.empty()) {
        screen.writeText(0, 3, fitLine(detail, width), dimStyle);
      }
      screen.writeText(0, 5,
                       fitLine("Enter: play audio only  Esc/Q: return", width),
                       dimStyle);
      screen.draw();
      while (input.poll(ev)) {
        if (ev.type == InputEvent::Type::Key) {
          if (ev.key.vk == VK_RETURN) return true;
          if (ev.key.vk == VK_ESCAPE || ev.key.vk == 'Q' || ev.key.ch == 'q' ||
              ev.key.ch == 'Q') {
            return false;
          }
        }
        if (ev.type == InputEvent::Type::Resize) {
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  };

  PerfLog perfLog;
  std::string logError;
  std::filesystem::path logPath =
      std::filesystem::current_path() / "radioify.log";
  configureFfmpegVideoLog(logPath);
  if (!perfLogOpen(&perfLog, logPath, &logError)) {
    bool ok = showError("Failed to open timing log file.", logError);
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }
  perfLogAppendf(&perfLog, "video_start file=%s",
                 toUtf8String(file.filename()).c_str());

  // Helper to append simple timing lines from worker threads where direct
  // access to `perfLog` may not be available due to capture/scope issues.
  auto appendTiming = [&](const std::string& s) {
#if RADIOIFY_ENABLE_TIMING_LOG
    std::ofstream f(logPath, std::ios::app);
    if (f) f << radioifyLogTimestamp() << " " << s << "\n";
#else
    (void)s;
#endif
  };
  auto appendTimingFmt = [&](const char* fmt, ...) {
#if RADIOIFY_ENABLE_TIMING_LOG
    if (!fmt) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int written = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (written <= 0) return;
    size_t count = static_cast<size_t>(written);
    if (count >= sizeof(buf)) {
      count = sizeof(buf) - 1;
    }
    appendTiming(std::string(buf, count));
#else
    (void)fmt;
#endif
  };

  auto appendVideoError = [&](const std::string& message,
                              const std::string& detail) {
#if RADIOIFY_ENABLE_VIDEO_ERROR_LOG
    std::string line = message;
    std::string extra = detail;
    if (!extra.empty() && extra == line) {
      extra.clear();
    }
    if (line.empty() && !extra.empty()) {
      line = extra;
      extra.clear();
    }
    if (line.empty()) {
      line = "Video playback error.";
    }
    std::string payload = "video_error msg=" + line;
    if (!extra.empty()) {
      payload += " detail=" + extra;
    }
    std::ofstream f(logPath, std::ios::app);
    if (f) f << radioifyLogTimestamp() << " " << payload << "\n";
#else
    (void)message;
    (void)detail;
#endif
  };

  auto appendVideoWarning = [&](const std::string& message) {
#if RADIOIFY_ENABLE_VIDEO_ERROR_LOG
    std::string line = message.empty() ? "Video warning." : message;
    std::string payload = "video_warning msg=" + line;
    std::ofstream f(logPath, std::ios::app);
    if (f) f << radioifyLogTimestamp() << " " << payload << "\n";
#else
    (void)message;
#endif
  };

  auto reportVideoError = [&](const std::string& message,
                              const std::string& detail) -> bool {
    appendVideoError(message, detail);
    return true;
  };

  std::mutex initMutex;
  std::condition_variable initCv;
  bool initDone = false;
  bool initOk = false;
  std::string initError;
  int sourceWidth = 0;
  int sourceHeight = 0;
  int64_t sourceDuration100ns = 0;

  std::mutex decodeErrorMutex;
  std::string decodeError;
  std::atomic<bool> decodeFailed{false};

  bool audioOk = false;
  bool audioStarting = enableAudio;
  std::atomic<bool> audioStartDone{false};
  std::atomic<bool> audioStartOk{false};
  Clock masterClock;
  std::atomic<int> serialCounter{1};
  std::atomic<int> currentSerial{1};
  bool running = true;
  bool videoEnded = false;
  bool redraw = true;
  AsciiArt art;
  GpuAsciiRenderer& gpuRenderer = sharedGpuRenderer();
  bool gpuAvailable = true;
  VideoFrame* frame = nullptr;
  size_t currentFrameIndex = 0;
  bool haveFrame = false;
  struct QueuedFrame {
    size_t poolIndex = 0;
    int64_t ptsUs = 0;
    int64_t durationUs = 0;
    uint64_t serial = 0;
    VideoReadInfo info{};
    double decodeMs = 0.0;
  };
  int cachedWidth = -1;
  int cachedMaxHeight = -1;
  int cachedFrameWidth = -1;
  int cachedFrameHeight = -1;
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;
  bool renderFailed = false;
  std::string renderFailMessage;
  std::string renderFailDetail;
  std::mutex scaleWarningMutex;
  std::string scaleWarning;
  std::atomic<bool> scaleWarningPending{false};
  int decoderTargetW = 0;
  int decoderTargetH = 0;
  int64_t fallbackFrameDurationUs = 0;
  int requestedTargetW = 0;
  int requestedTargetH = 0;
  bool pendingResize = false;
  bool forceRefreshArt = false;
  std::atomic<int> pendingSeekSerial{0};
  double pendingSeekTargetSec = 0.0;
  bool userPaused = false;
  int64_t lastPresentedPtsUs = 0;
  int64_t lastPresentedDurationUs = 0;

  std::mutex queueMutex;
  std::condition_variable queueCv;
  std::deque<QueuedFrame> frameQueue;
  constexpr size_t kMaxQueuedFrames = 32;
  constexpr size_t kInvalidPoolIndex = static_cast<size_t>(-1);
  std::vector<VideoFrame> framePool;
  std::deque<size_t> freeFrames;
  std::atomic<size_t> maxQueue{3};
  constexpr bool kEnableSoftwareFallback = false;
  const bool allowDecoderScale = enableAscii;
  struct DecodeCommand {
    bool resize = false;
    int targetW = 0;
    int targetH = 0;
    bool seek = false;
    int64_t seekTs = 0;
    int serial = 0;
  };
  std::mutex commandMutex;
  DecodeCommand pendingCommand;
  bool commandPending = false;
  std::atomic<bool> commandPendingAtomic{false};
  std::atomic<uint64_t> resizeEpoch{0};
  std::atomic<int> resizeTargetW{0};
  std::atomic<int> resizeTargetH{0};
  std::atomic<bool> pipelineRunning{false};
  std::atomic<bool> decodeEnded{false};
  std::atomic<bool> demuxEnded{false};
  std::atomic<bool> audioDecodeEnded{false};
  std::atomic<uint64_t> readCalls{0};
  std::atomic<uint64_t> queueDrops{0};
  DemuxInterrupt demuxInterrupt;
  std::thread demuxThread;
  std::thread videoThread;
  std::thread audioThread;
  int lastPresentPopped = 0;
  int lastPresentDropped = 0;
  PacketQueue videoPackets;
  PacketQueue audioPackets;
  DemuxContext demux{};
  VideoDecodeContext videoDec{};
  AudioDecodeContext audioDec{};
  framePool.resize(kMaxQueuedFrames + 1);
  for (size_t i = 0; i < framePool.size(); ++i) {
    freeFrames.push_back(i);
  }
  auto clearQueue = [&]() {
    std::lock_guard<std::mutex> lock(queueMutex);
    while (!frameQueue.empty()) {
      freeFrames.push_back(frameQueue.front().poolIndex);
      frameQueue.pop_front();
    }
  };

  auto setDecodeError = [&](const std::string& message) {
    std::lock_guard<std::mutex> lock(decodeErrorMutex);
    decodeError = message;
    decodeFailed.store(true);
  };

  auto setScaleWarning = [&](const std::string& message) {
    {
      std::lock_guard<std::mutex> lock(scaleWarningMutex);
      scaleWarning = message;
    }
    scaleWarningPending.store(true);
  };

  auto getScaleWarning = [&]() {
    std::lock_guard<std::mutex> lock(scaleWarningMutex);
    return scaleWarning;
  };

  auto getDecodeError = [&]() {
    std::lock_guard<std::mutex> lock(decodeErrorMutex);
    return decodeError;
  };

  auto issueCommand = [&](const DecodeCommand& command) {
    {
      std::lock_guard<std::mutex> lock(commandMutex);
      pendingCommand = command;
      commandPending = true;
    }
    commandPendingAtomic.store(true);
    queueCv.notify_all();
  };

  auto tryPopFrame = [&](QueuedFrame& out, uint64_t serial) -> bool {
    std::lock_guard<std::mutex> lock(queueMutex);
    while (!frameQueue.empty()) {
      QueuedFrame candidate = std::move(frameQueue.front());
      frameQueue.pop_front();
      if (serial == 0 || candidate.serial == serial) {
        out = std::move(candidate);
        return true;
      }
      freeFrames.push_back(candidate.poolIndex);
    }
    return false;
  };

  auto computeTargetSizeForSource = [&](int width, int height, int srcW,
                                        int srcH, bool showSubtitle) {
    int headerLines = showSubtitle ? 1 : 0;
    const int footerLines = 0;
    int maxHeight = std::max(1, height - headerLines - footerLines);
    int maxOutW = std::max(1, width - 8);
    int safeSrcW = std::max(1, srcW);
    int safeSrcH = std::max(1, srcH);
    int outW = std::max(1, std::min(maxOutW, safeSrcW / 2));
    int outH = static_cast<int>(
        std::lround(outW * (static_cast<float>(safeSrcH) / safeSrcW) / 2.0f));
    outH = std::max(1, std::min(outH, safeSrcH / 4));
    if (maxHeight > 0) outH = std::min(outH, maxHeight);
    int targetW = std::max(2, outW * 2);
    int targetH = std::max(4, outH * 4);
    if (targetW & 1) ++targetW;
    if (targetH & 1) ++targetH;
    if (srcW > 0) targetW = std::min(targetW, srcW & ~1);
    if (srcH > 0) targetH = std::min(targetH, srcH & ~1);
    targetW = std::max(2, targetW);
    targetH = std::max(4, targetH);

    const int kMaxDecodeWidth = 1024;
    const int kMaxDecodeHeight = 768;
    if (targetW > kMaxDecodeWidth || targetH > kMaxDecodeHeight) {
      double scaleW = static_cast<double>(kMaxDecodeWidth) / targetW;
      double scaleH = static_cast<double>(kMaxDecodeHeight) / targetH;
      double scale = std::min(scaleW, scaleH);
      targetW = static_cast<int>(std::lround(targetW * scale));
      targetH = static_cast<int>(std::lround(targetH * scale));
      targetW = std::min(targetW, kMaxDecodeWidth);
      targetH = std::min(targetH, kMaxDecodeHeight);
      targetW &= ~1;
      targetH &= ~1;
      targetW = std::max(2, targetW);
      targetH = std::max(4, targetH);
    }
    return std::pair<int, int>(targetW, targetH);
  };

  auto computeTargetSize = [&](int width, int height) {
    bool showSubtitle = !audioOk;
    return computeTargetSizeForSource(width, height, sourceWidth, sourceHeight,
                                      showSubtitle);
  };

  auto computeAsciiOutputSize = [&](int maxWidth, int maxHeight, int srcW,
                                    int srcH) {
    int safeSrcW = std::max(1, srcW);
    int safeSrcH = std::max(1, srcH);
    int maxOutW = std::max(1, maxWidth - 8);
    int outW = std::max(1, std::min(maxOutW, safeSrcW / 2));
    int outH = static_cast<int>(
        std::lround(outW * (static_cast<float>(safeSrcH) / safeSrcW) / 2.0f));
    outH = std::max(1, std::min(outH, safeSrcH / 4));
    if (maxHeight > 0) outH = std::min(outH, maxHeight);
    return std::pair<int, int>(outW, outH);
  };

  auto requestTargetSize = [&](int width, int height) -> bool {
    auto [targetW, targetH] = computeTargetSize(width, height);
    if (targetW == requestedTargetW && targetH == requestedTargetH) {
      return false;
    }
    requestedTargetW = targetW;
    requestedTargetH = targetH;
    DecodeCommand cmd;
    cmd.resize = true;
    cmd.targetW = targetW;
    cmd.targetH = targetH;
    issueCommand(cmd);
    return true;
  };

  auto setCurrentFrame = [&](size_t poolIndex) {
    size_t releaseIndex = kInvalidPoolIndex;
    if (haveFrame) {
      releaseIndex = currentFrameIndex;
    }
    currentFrameIndex = poolIndex;
    frame = &framePool[currentFrameIndex];
    haveFrame = true;
    if (releaseIndex != kInvalidPoolIndex) {
      std::lock_guard<std::mutex> lock(queueMutex);
      freeFrames.push_back(releaseIndex);
      queueCv.notify_all();
    }
  };

  screen.updateSize();
  const int initScreenWidth = std::max(20, screen.width());
  const int initScreenHeight = std::max(10, screen.height());

  auto stopPipeline = [&]() {
    pipelineRunning.store(false);
    commandPendingAtomic.store(false);
    audioStreamSetClock(nullptr);
    videoPackets.abortQueue();
    audioPackets.abortQueue();
    queueCv.notify_all();
    if (demuxThread.joinable()) {
      demuxThread.join();
    }
    if (videoThread.joinable()) {
      videoThread.join();
    }
    if (audioThread.joinable()) {
      audioThread.join();
    }
    videoPackets.flush();
    audioPackets.flush();
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
  };

  auto startPipeline = [&]() {
    if (pipelineRunning.load()) return;
    pipelineRunning.store(true);
    decodeEnded.store(false);
    demuxEnded.store(false);
    audioDecodeEnded.store(false);
    decodeFailed.store(false);
    {
      std::lock_guard<std::mutex> lock(decodeErrorMutex);
      decodeError.clear();
    }
    resizeEpoch.store(0);
    serialCounter.store(1);
    currentSerial.store(1);
    pendingSeekSerial.store(0);
    pendingSeekTargetSec = 0.0;
    audioStartDone.store(false);
    audioStartOk.store(false);
    videoPackets.init(32 * 1024 * 1024);
    audioPackets.init(8 * 1024 * 1024);
    demuxInterrupt.running = &pipelineRunning;
    demuxInterrupt.commandPending = &commandPendingAtomic;

    demuxThread = std::thread([&]() {
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
          initDone = true;
          initOk = false;
          initError = message;
        }
        initCv.notify_all();
        audioStartOk.store(false);
        audioStartDone.store(true);
        pipelineRunning.store(false);
        videoPackets.abortQueue();
        audioPackets.abortQueue();
        queueCv.notify_all();
        if (roInit) {
          RoUninitialize();
        }
        if (comInit) {
          CoUninitialize();
        }
      };

      if (!initDemuxer(file, &demux, &error, &demuxInterrupt)) {
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

      if (allowDecoderScale && !videoDec.useSharedDevice) {
        auto [targetW, targetH] = computeTargetSizeForSource(
            initScreenWidth, initScreenHeight, videoDec.width,
            videoDec.height, false);
        videoDec.targetW = targetW;
        videoDec.targetH = targetH;
      }

      sourceWidth = videoDec.width;
      sourceHeight = videoDec.height;
      int64_t durationUs = demux.durationUs;
      if (durationUs <= 0 && demux.videoStreamIndex >= 0) {
        AVStream* stream = demux.fmt->streams[demux.videoStreamIndex];
        if (stream && stream->duration > 0) {
          durationUs = av_rescale_q(stream->duration, stream->time_base,
                                    AVRational{1, AV_TIME_BASE});
        }
      }
      sourceDuration100ns = durationUs > 0 ? durationUs * 10 : 0;
      double estimatedFrameDurationSec = 0.0;
      if (demux.videoStreamIndex >= 0 && demux.fmt) {
        AVStream* stream = demux.fmt->streams[demux.videoStreamIndex];
        if (stream) {
          AVRational rate = av_guess_frame_rate(demux.fmt, stream, nullptr);
          if (rate.num > 0 && rate.den > 0) {
            estimatedFrameDurationSec = av_q2d(av_inv_q(rate));
          } else if (stream->avg_frame_rate.num > 0 &&
                     stream->avg_frame_rate.den > 0) {
            estimatedFrameDurationSec =
                av_q2d(av_inv_q(stream->avg_frame_rate));
          } else if (stream->r_frame_rate.num > 0 &&
                     stream->r_frame_rate.den > 0) {
            estimatedFrameDurationSec =
                av_q2d(av_inv_q(stream->r_frame_rate));
          }
        }
      }
      if (estimatedFrameDurationSec <= 0.0) {
        estimatedFrameDurationSec = 1.0 / 30.0;
      }
      fallbackFrameDurationUs = static_cast<int64_t>(
          estimatedFrameDurationSec * 1000000.0);
      {
        std::lock_guard<std::mutex> lock(initMutex);
        initDone = true;
        initOk = true;
        initError.clear();
      }
      initCv.notify_all();

      videoThread = std::thread([&]() {
        HRESULT vhr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool vcomInit = SUCCEEDED(vhr);
        HRESULT vroHr = RoInitialize(RO_INIT_MULTITHREADED);
        bool vroInit = SUCCEEDED(vroHr);

        uint64_t decoderSerial = 0;
        uint64_t lastResizeEpoch = 0;
        bool inputEof = false;
        int64_t lastPtsUs = 0;
        int64_t lastDurationUs =
            (fallbackFrameDurationUs > 0) ? fallbackFrameDurationUs : 33333;
        QueuedPacket pendingPacket{};
        bool hasPendingPacket = false;

        while (pipelineRunning.load()) {
          uint64_t newResizeEpoch = resizeEpoch.load();
          if (newResizeEpoch != lastResizeEpoch) {
            int targetW = resizeTargetW.load();
            int targetH = resizeTargetH.load();
            if (targetW > 0 && targetH > 0) {
              videoDec.targetW = targetW;
              videoDec.targetH = targetH;
              if (!videoDec.useSharedDevice) {
                clampTargetSize(videoDec.targetW, videoDec.targetH);
              }
            }
            lastResizeEpoch = newResizeEpoch;
            clearQueue();
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
            clearQueue();
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
            int send = avcodec_send_packet(videoDec.codec,
                                           &pendingPacket.pkt);
            if (send != AVERROR(EAGAIN)) {
              av_packet_unref(&pendingPacket.pkt);
              hasPendingPacket = false;
              if (send < 0 && send != AVERROR_EOF) {
                continue;
              }
            }
          }

          while (pipelineRunning.load()) {
            int recv = avcodec_receive_frame(videoDec.codec,
                                             videoDec.frame);
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
              setDecodeError("Failed to decode video frame.");
              decodeEnded.store(true);
              break;
            }

            size_t poolIndex = kInvalidPoolIndex;
            {
              std::unique_lock<std::mutex> lock(queueMutex);
              size_t queueLimit = maxQueue.load(std::memory_order_relaxed);
              if (frameQueue.size() >= queueLimit || freeFrames.empty()) {
                queueCv.wait_for(lock, std::chrono::milliseconds(30), [&]() {
                  return !pipelineRunning.load() ||
                         (frameQueue.size() <
                              maxQueue.load(std::memory_order_relaxed) &&
                          !freeFrames.empty());
                });
                if (!pipelineRunning.load()) {
                  av_frame_unref(videoDec.frame);
                  break;
                }
                if (frameQueue.size() >= queueLimit || freeFrames.empty()) {
                  av_frame_unref(videoDec.frame);
                  continue;
                }
              }
              poolIndex = freeFrames.front();
              freeFrames.pop_front();
            }

            VideoFrame& decodedFrame = framePool[poolIndex];
            VideoReadInfo info{};
            auto decodeStart = std::chrono::steady_clock::now();
            bool ok = emitVideoFrame(&videoDec, decodedFrame, &info,
                                     videoDec.frame, enableAscii,
                                     videoDec.useSharedDevice);
            av_frame_unref(videoDec.frame);
            if (!ok) {
              std::lock_guard<std::mutex> lock(queueMutex);
              freeFrames.push_back(poolIndex);
              continue;
            }

            auto decodeEnd = std::chrono::steady_clock::now();
            double decodeMs =
                std::chrono::duration<double, std::milli>(decodeEnd -
                                                          decodeStart)
                    .count();
            readCalls.fetch_add(1, std::memory_order_relaxed);

            int64_t ptsUs = 0;
            if (decodedFrame.timestamp100ns > 0) {
              ptsUs = decodedFrame.timestamp100ns / 10;
            } else if (lastPtsUs > 0) {
              ptsUs = lastPtsUs + lastDurationUs;
            }
            int64_t durationUs = 0;
            if (info.duration100ns > 0) {
              durationUs = info.duration100ns / 10;
            } else {
              durationUs = lastDurationUs;
            }
            if (durationUs <= 0) {
              durationUs =
                  (fallbackFrameDurationUs > 0) ? fallbackFrameDurationUs : 33333;
            }
            lastPtsUs = ptsUs;
            lastDurationUs = durationUs;

            {
              std::lock_guard<std::mutex> lock(queueMutex);
              size_t queueLimit = maxQueue.load(std::memory_order_relaxed);
              if (frameQueue.size() >= queueLimit) {
                freeFrames.push_back(frameQueue.front().poolIndex);
                frameQueue.pop_front();
                queueDrops.fetch_add(1, std::memory_order_relaxed);
              }
              frameQueue.push_back(
                  QueuedFrame{poolIndex, ptsUs, durationUs,
                              static_cast<uint64_t>(decoderSerial), info,
                              decodeMs});
            }
            queueCv.notify_one();
          }
        }

        decodeEnded.store(true);
        queueCv.notify_all();
        if (vroInit) {
          RoUninitialize();
        }
        if (vcomInit) {
          CoUninitialize();
        }
      });

      audioThread = std::thread([&]() {
        bool audioReady = false;
        if (enableAudio && demux.audioStreamIndex >= 0) {
          uint64_t totalFrames = 0;
          AVStream* stream = demux.fmt->streams[demux.audioStreamIndex];
          if (stream && stream->duration > 0) {
            int64_t us = av_rescale_q(stream->duration, stream->time_base,
                                      AVRational{1, AV_TIME_BASE});
            if (us > 0) {
              totalFrames =
                  static_cast<uint64_t>(rescaleToFrames(
                      us, AVRational{1, AV_TIME_BASE}, 48000));
            }
          } else if (demux.durationUs > 0) {
            totalFrames =
                static_cast<uint64_t>(rescaleToFrames(
                    demux.durationUs, AVRational{1, AV_TIME_BASE}, 48000));
          }
          if (audioStartStream(totalFrames)) {
            audioStreamSetClock(&masterClock);
            int serial = pendingSeekSerial.load();
            if (serial == 0) {
              serial = currentSerial.load();
            }
            audioStreamFlush(serial);
            AudioPerfStats stats = audioGetPerfStats();
            uint32_t outRate = stats.sampleRate ? stats.sampleRate : 48000;
            uint32_t outChannels = stats.channels ? stats.channels : 2;
            if (initAudioDecoder(demux, outRate, outChannels, &audioDec,
                                 &error)) {
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

        QueuedPacket pendingPacket{};
        bool hasPendingPacket = false;
        bool inputEof = false;
        uint64_t decoderSerial = 0;
        while (pipelineRunning.load()) {
          if (!hasPendingPacket) {
            if (!audioPackets.pop(&pendingPacket)) {
              break;
            }
            hasPendingPacket = true;
          }

          if (pendingPacket.flush) {
            avcodec_flush_buffers(audioDec.codec);
            audioDec.writePosFrames = 0;
            audioDec.nextPtsUs = 0;
            audioDec.nextPtsValid = false;
            decoderSerial = pendingPacket.serial;
            audioStreamFlush(static_cast<int>(decoderSerial));
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
            int send = avcodec_send_packet(audioDec.codec,
                                           &pendingPacket.pkt);
            if (send != AVERROR(EAGAIN)) {
              av_packet_unref(&pendingPacket.pkt);
              hasPendingPacket = false;
              if (send < 0 && send != AVERROR_EOF) {
                continue;
              }
            }
          }

          while (pipelineRunning.load()) {
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
            int64_t durationUs = static_cast<int64_t>(
                (static_cast<uint64_t>(converted) * 1000000ULL) /
                static_cast<uint64_t>(audioDec.outRate));
            if (ptsUs == AV_NOPTS_VALUE) {
              if (audioDec.nextPtsValid) {
                ptsUs = audioDec.nextPtsUs;
              } else {
                ptsUs = static_cast<int64_t>(
                    (static_cast<uint64_t>(audioDec.writePosFrames) *
                     1000000ULL) /
                    static_cast<uint64_t>(audioDec.outRate));
              }
            }
            audioDec.nextPtsUs = ptsUs + durationUs;
            audioDec.nextPtsValid = true;
            audioDec.writePosFrames += static_cast<int64_t>(converted);
            if (!audioStreamPushFrame(audioDec.convertBuffer.data(),
                                      static_cast<size_t>(converted), ptsUs,
                                      durationUs,
                                      static_cast<int>(decoderSerial))) {
              break;
            }
          }
        }
        audioStreamSetEnd(true);
        audioDecodeEnded.store(true);
      });

      uint64_t serial = static_cast<uint64_t>(currentSerial.load());
      AVPacket pkt;
      av_init_packet(&pkt);
      bool demuxAtEof = false;
      while (pipelineRunning.load()) {
        DecodeCommand command;
        bool hasCommand = false;
        {
          std::lock_guard<std::mutex> lock(commandMutex);
          if (commandPending) {
            command = pendingCommand;
            commandPending = false;
            hasCommand = true;
            commandPendingAtomic.store(false);
          }
        }
        if (hasCommand) {
          if (command.resize) {
            resizeTargetW.store(command.targetW);
            resizeTargetH.store(command.targetH);
            uint64_t resizeToken = resizeEpoch.fetch_add(1) + 1;
            clearQueue();
            appendTimingFmt("demux_resize token=%llu target=%dx%d",
                            static_cast<unsigned long long>(resizeToken),
                            command.targetW, command.targetH);
          }
          if (command.seek) {
            int64_t targetUs = command.seekTs / 10;
            targetUs += demux.formatStartUs;
            if (targetUs < 0) targetUs = 0;
            appendTimingFmt(
                "demux_seek serial=%d target_ts=%lld target_us=%lld",
                command.serial,
                static_cast<long long>(command.seekTs),
                static_cast<long long>(targetUs));
            int seekRes = avformat_seek_file(demux.fmt, -1, INT64_MIN,
                                             targetUs, INT64_MAX,
                                             AVSEEK_FLAG_BACKWARD);
            if (seekRes < 0 && demux.videoStreamIndex >= 0) {
              int64_t targetStream =
                  av_rescale_q(targetUs, AVRational{1, AV_TIME_BASE},
                               demux.videoTimeBase);
              seekRes = av_seek_frame(demux.fmt, demux.videoStreamIndex,
                                      targetStream, AVSEEK_FLAG_BACKWARD);
            }
            appendTimingFmt("demux_seek_result serial=%d res=%d",
                            command.serial,
                            seekRes);
            if (seekRes < 0) {
              setDecodeError("Failed to seek video.");
              pendingSeekSerial.store(0);
              audioStreamFlush(currentSerial.load());
            } else {
              avformat_flush(demux.fmt);
              serial = static_cast<uint64_t>(command.serial);
              currentSerial.store(static_cast<int>(serial));
              demuxEnded.store(false);
              demuxAtEof = false;
              decodeEnded.store(false);
              videoPackets.flush(serial);
              audioPackets.flush(serial);
              audioStreamFlush(static_cast<int>(serial));
              clearQueue();
            }
          }
          queueCv.notify_all();
        }

        if (demuxAtEof) {
          std::unique_lock<std::mutex> lock(queueMutex);
          queueCv.wait_for(lock, std::chrono::milliseconds(20), [&]() {
            return !pipelineRunning.load() || commandPendingAtomic.load();
          });
          continue;
        }

        int read = av_read_frame(demux.fmt, &pkt);
        if (read == AVERROR_EXIT || read == AVERROR(EINTR)) {
          if (!pipelineRunning.load()) {
            break;
          }
          continue;
        }
        if (read < 0) {
          if (!demuxAtEof) {
            demuxEnded.store(true);
            videoPackets.pushEof(serial);
            audioPackets.pushEof(serial);
            appendTimingFmt("demux_read_end err=%d", read);
            demuxAtEof = true;
          }
          continue;
        }

        if (pkt.stream_index == demux.videoStreamIndex) {
          if (!videoPackets.pushPacket(&pkt, serial)) {
            av_packet_unref(&pkt);
            break;
          }
        } else if (pkt.stream_index == demux.audioStreamIndex) {
          if (!audioPackets.pushPacket(&pkt, serial)) {
            av_packet_unref(&pkt);
            break;
          }
        }
        av_packet_unref(&pkt);
      }

      demuxEnded.store(true);
      queueCv.notify_all();
      if (roInit) {
        RoUninitialize();
      }
      if (comInit) {
        CoUninitialize();
      }
    });
  };

  constexpr auto kPrepRedrawInterval = std::chrono::milliseconds(120);
  constexpr double kPrepPulseSeconds = 1.6;
  auto renderPreparingScreen = [&](double progress) {
    screen.updateSize();
    int width = std::max(20, screen.width());
    int height = std::max(10, screen.height());
    screen.clear(baseStyle);
    std::string title = "Video: " + toUtf8String(file.filename());
    screen.writeText(0, 0, fitLine(title, width), accentStyle);
    std::string message = "Preparing video playback...";
    int msgLine = std::clamp(height / 2, 1, std::max(1, height - 2));
    int msgWidth = utf8CodepointCount(message);
    if (msgWidth >= width) {
      screen.writeText(0, msgLine, fitLine(message, width), dimStyle);
    } else {
      int msgX = (width - msgWidth) / 2;
      screen.writeText(msgX, msgLine, message, dimStyle);
    }
    int barWidth = std::min(32, width - 6);
    int barLine = msgLine + 1;
    if (barWidth >= 5 && barLine < height) {
      int barX = std::max(0, (width - (barWidth + 2)) / 2);
      screen.writeChar(barX, barLine, L'|', progressFrameStyle);
      auto barCells = renderProgressBarCells(progress, barWidth,
                                             progressEmptyStyle, progressStart,
                                             progressEnd);
      for (int i = 0; i < barWidth; ++i) {
        const auto& cell = barCells[static_cast<size_t>(i)];
        screen.writeChar(barX + 1 + i, barLine, cell.ch, cell.style);
      }
      screen.writeChar(barX + 1 + barWidth, barLine, L'|',
                       progressFrameStyle);
    }
    screen.draw();
  };

  startPipeline();
  auto initStart = std::chrono::steady_clock::now();
  auto lastInitDraw = std::chrono::steady_clock::time_point::min();
  while (running) {
    {
      std::unique_lock<std::mutex> lock(initMutex);
      if (initDone) {
        break;
      }
      initCv.wait_for(lock, std::chrono::milliseconds(30),
                      [&]() { return initDone; });
      if (initDone) {
        break;
      }
    }
    auto now = std::chrono::steady_clock::now();
    if (now - lastInitDraw >= kPrepRedrawInterval) {
      double elapsed = std::chrono::duration<double>(now - initStart).count();
      double phase = std::fmod(elapsed, kPrepPulseSeconds);
      double ratio = (phase <= (kPrepPulseSeconds * 0.5))
                         ? (phase / (kPrepPulseSeconds * 0.5))
                         : ((kPrepPulseSeconds - phase) /
                            (kPrepPulseSeconds * 0.5));
      renderPreparingScreen(ratio);
      lastInitDraw = now;
    }
    InputEvent ev{};
    while (input.poll(ev)) {
      if (ev.type == InputEvent::Type::Key) {
        const KeyEvent& key = ev.key;
        const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
        bool ctrl = (key.control & ctrlMask) != 0;
        if ((key.vk == 'C' || key.ch == 'c' || key.ch == 'C') && ctrl) {
          running = false;
          break;
        }
        if (key.vk == VK_ESCAPE || key.vk == 'Q' || key.ch == 'q' ||
            key.ch == 'Q') {
          running = false;
          break;
        }
      } else if (ev.type == InputEvent::Type::Resize) {
        lastInitDraw = std::chrono::steady_clock::time_point::min();
      }
    }
  }
  if (!running) {
    stopPipeline();
    if (audioOk) audioStop();
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return true;
  }
  if (!initOk) {
    stopPipeline();
    if (initError.rfind("No video stream found", 0) == 0) {
      if (!enableAudio) {
        bool ok =
            showError("No video stream found.", "Audio playback is disabled.");
        finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
        return ok;
      }
      bool playAudio = showAudioFallbackPrompt(
          "No video stream found.", "This file can be played as audio only.");
      finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
      return playAudio ? false : true;
    }
    if (initError.empty()) {
      initError = "Failed to open video.";
    }
    bool ok = showError(initError, "");
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }

  VideoReadInfo firstInfo{};

  QueuedFrame firstQueued{};
  bool haveFirstQueued = false;
  constexpr size_t kPrepMinBufferedFrames = 2;
  constexpr auto kPrepMaxWait = std::chrono::milliseconds(600);
  auto prepStart = std::chrono::steady_clock::now();
  auto lastPrepDraw = std::chrono::steady_clock::time_point::min();
  auto lastPrepLog = std::chrono::steady_clock::time_point::min();
  while (running) {
    if (!haveFirstQueued &&
        tryPopFrame(firstQueued, static_cast<uint64_t>(currentSerial.load()))) {
      haveFirstQueued = true;
    }
    if (haveFirstQueued) {
      size_t bufferedFrames = 0;
      {
        std::lock_guard<std::mutex> lock(queueMutex);
        bufferedFrames = frameQueue.size();
      }
      bufferedFrames += 1;
      if (bufferedFrames >= kPrepMinBufferedFrames ||
          std::chrono::steady_clock::now() - prepStart >= kPrepMaxWait) {
        break;
      }
    }
    if (decodeEnded.load()) {
      break;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - lastPrepLog >= std::chrono::milliseconds(500)) {
      size_t queued = 0;
      {
        std::lock_guard<std::mutex> lock(queueMutex);
        queued = frameQueue.size();
      }
      IoCacheStats ioStats{};
      bool ioStatsValid = false;
      if (demux.io) {
        ioStats = demux.io->stats();
        ioStatsValid = true;
      }
      perfLogAppendf(
          &perfLog,
          "prep_wait have_first=%d queued=%zu vpkts=%zu apkts=%zu io=%zu/%zu io_eof=%d demux_end=%d decode_end=%d",
          haveFirstQueued ? 1 : 0, queued, videoPackets.size(),
          audioPackets.size(), ioStatsValid ? ioStats.size : 0,
          ioStatsValid ? ioStats.capacity : 0,
          ioStatsValid && ioStats.eof ? 1 : 0, demuxEnded.load() ? 1 : 0,
          decodeEnded.load() ? 1 : 0);
      lastPrepLog = now;
    }
    if (now - lastPrepDraw >= kPrepRedrawInterval) {
      double elapsed = std::chrono::duration<double>(now - prepStart).count();
      double phase = std::fmod(elapsed, kPrepPulseSeconds);
      double halfPulse = kPrepPulseSeconds * 0.5;
      double ratio =
          (phase <= halfPulse) ? (phase / halfPulse)
                               : ((kPrepPulseSeconds - phase) / halfPulse);
      renderPreparingScreen(ratio);
      lastPrepDraw = now;
    }
    InputEvent ev{};
    while (input.poll(ev)) {
      if (ev.type == InputEvent::Type::Key) {
        const KeyEvent& key = ev.key;
        const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
        bool ctrl = (key.control & ctrlMask) != 0;
        if ((key.vk == 'C' || key.ch == 'c' || key.ch == 'C') && ctrl) {
          running = false;
          break;
        }
        if (key.vk == VK_ESCAPE || key.vk == 'Q' || key.ch == 'q' ||
            key.ch == 'Q') {
          running = false;
          break;
        }
      } else if (ev.type == InputEvent::Type::Resize) {
        lastPrepDraw = std::chrono::steady_clock::time_point::min();
      }
    }
    if (!running) {
      break;
    }
    std::unique_lock<std::mutex> lock(queueMutex);
    queueCv.wait_for(lock, std::chrono::milliseconds(30), [&]() {
      return !frameQueue.empty() || decodeEnded.load();
    });
  }
  if (!running) {
    stopPipeline();
    if (audioOk) audioStop();
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return true;
  }
  if (!haveFirstQueued) {
    stopPipeline();
    if (audioOk) audioStop();
    std::string fail = getDecodeError();
    if (fail.empty()) {
      fail = "No video frames found.";
    }
    bool ok = showError(fail, "");
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }
  setCurrentFrame(firstQueued.poolIndex);
  firstInfo = firstQueued.info;
  decoderTargetW = frame->width;
  decoderTargetH = frame->height;
  requestedTargetW = decoderTargetW;
  requestedTargetH = decoderTargetH;
  perfLogAppendf(
      &perfLog,
      "first_frame ts100ns=%lld dur100ns=%lld flags=0x%X rec=%u ehr=0x%08X "
      "size=%dx%d",
      static_cast<long long>(frame->timestamp100ns),
      static_cast<long long>(firstInfo.duration100ns), firstInfo.flags,
      firstInfo.recoveries, firstInfo.errorHr, frame->width, frame->height);

  const double ticksToSeconds = 1.0 / 10000000.0;
  const double secondsToTicks = 10000000.0;
  const double kVideoBufferHighSec = 0.75;
  int64_t firstTs = frame->timestamp100ns;
  int64_t firstPtsUs = firstTs / 10;
  lastPresentedPtsUs = firstPtsUs;
  lastPresentedDurationUs = firstInfo.duration100ns > 0
                                ? firstInfo.duration100ns / 10
                                : fallbackFrameDurationUs;
  if (lastPresentedDurationUs <= 0) {
    lastPresentedDurationUs =
        (fallbackFrameDurationUs > 0) ? fallbackFrameDurationUs : 33333;
  }
  masterClock.set(lastPresentedPtsUs, nowUs());
  auto updateMaxQueue = [&](int64_t frameDurUs) {
    if (frameDurUs <= 0) return;
    double frameDurSec = static_cast<double>(frameDurUs) / 1000000.0;
    size_t needed =
        static_cast<size_t>(std::ceil(kVideoBufferHighSec / frameDurSec)) + 2;
    if (needed < 3) needed = 3;
    constexpr size_t kMinQueuedFrames = 6;
    if (needed < kMinQueuedFrames) needed = kMinQueuedFrames;
    if (needed > kMaxQueuedFrames) needed = kMaxQueuedFrames;
    maxQueue.store(needed, std::memory_order_relaxed);
  };
  updateMaxQueue(lastPresentedDurationUs);

  auto totalDurationSec = [&]() -> double {
    double total =
        sourceDuration100ns > 0 ? sourceDuration100ns * ticksToSeconds : -1.0;
    if (total <= 0.0 && audioOk) {
      total = audioGetTotalSec();
    }
    return total;
  };

  auto currentTimeSec = [&]() -> double {
    int64_t clockUs = masterClock.get(nowUs());
    if (clockUs <= 0 && lastPresentedPtsUs > 0) {
      clockUs = lastPresentedPtsUs;
    }
    if (clockUs < 0) clockUs = 0;
    return static_cast<double>(clockUs) / 1000000.0;
  };

  constexpr auto kProgressOverlayTimeout = std::chrono::milliseconds(2000);
  auto overlayUntil = std::chrono::steady_clock::time_point::min();
  auto triggerOverlay = [&]() {
    overlayUntil = std::chrono::steady_clock::now() + kProgressOverlayTimeout;
  };
  auto overlayVisible = [&]() {
    if (overlayUntil == std::chrono::steady_clock::time_point::min()) {
      return false;
    }
    return std::chrono::steady_clock::now() <= overlayUntil;
  };
  auto renderScreen = [&](bool refreshArt) {
    screen.updateSize();
    int width = std::max(20, screen.width());
    int height = std::max(10, screen.height());
    std::string subtitle;
    if (!audioOk && !audioStarting) {
      subtitle = enableAudio ? "Audio unavailable" : "Audio disabled";
    }
    int headerLines = 0;
    if (!subtitle.empty()) {
      headerLines += 1;
    }
    const int footerLines = 0;
    int artTop = headerLines;
    int maxHeight = std::max(1, height - headerLines - footerLines);

    double currentSec = currentTimeSec();
    double totalSec = totalDurationSec();
    if (totalSec > 0.0) {
      currentSec = std::clamp(currentSec, 0.0, totalSec);
    }
    double displaySec = currentSec;
    if (pendingSeekSerial.load() != 0 && totalSec > 0.0 &&
        std::isfinite(totalSec)) {
      displaySec = std::clamp(pendingSeekTargetSec, 0.0, totalSec);
    }
    bool waitingForAudio = audioOk && !audioStreamIsClockReady();
    bool audioStarved = audioOk && audioStreamIsStarved();
    bool waitingForVideo = false;
    {
      std::lock_guard<std::mutex> lock(queueMutex);
      waitingForVideo = frameQueue.empty();
    }
    bool allowFrame = haveFrame;
    auto waitingLabel = [&]() -> std::string {
      if (pendingSeekSerial.load() != 0) return "Seeking...";
      if (waitingForAudio) return "Waiting for audio...";
      if (audioStarved || waitingForVideo) return "Buffering video...";
      return "Waiting for video...";
    };

    bool sizeChanged =
        (width != cachedWidth || maxHeight != cachedMaxHeight ||
         frame->width != cachedFrameWidth || frame->height != cachedFrameHeight);
    if (enableAscii) {
      if (allowFrame && (refreshArt || sizeChanged)) {
        if (frame->width <= 0 || frame->height <= 0) {
          renderFailed = true;
          renderFailMessage = "Invalid video frame size.";
          renderFailDetail = "";
          return;
        }
        bool artOk = false;
        try {
          // Zero-copy GPU texture path (HWTexture from shared device decoder)
          if (frame->format == VideoPixelFormat::HWTexture && frame->hwTexture) {
            if (gpuAvailable) {
              auto [outW, outH] =
                  computeAsciiOutputSize(width, maxHeight, frame->width,
                                         frame->height);
              art.width = outW;
              art.height = outH;
              std::string gpuErr;
              
              bool renderRes = false;
              {
                   // Synchronize access to the shared D3D11 immediate context
                   std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
                   renderRes = gpuRenderer.RenderNV12Texture(frame->hwTexture.Get(), frame->hwTextureArrayIndex,
                                                 frame->width, frame->height,
                                                 frame->fullRange, frame->yuvMatrix,
                                                 frame->yuvTransfer, false, art, &gpuErr);
              }

              if (renderRes) {
                artOk = true;
                static bool hwTextureLogged = false;
                if (!hwTextureLogged) {
                  perfLogAppendf(&perfLog,
                                 "video_renderer_input format=hwtexture in=%dx%d out=%dx%d",
                                 frame->width, frame->height, outW, outH);
                  std::string hwDetail = gpuRenderer.lastNv12TextureDetail();
                  if (hwDetail.empty()) {
                    hwDetail = std::string("path=") +
                               gpuRenderer.lastNv12TexturePath();
                  }
                  appendTiming(std::string("video_renderer gpu_active=1 format=hwtexture ") +
                               hwDetail);
                  hwTextureLogged = true;
                }
              } else {
                gpuAvailable = false;
                appendVideoWarning("GPU renderer failed (HWTexture), falling back to CPU: " +
                                   gpuErr);
                throw std::runtime_error("ASCII-renderer GPU failure" + gpuErr);
              }
            }
          } else if (frame->format == VideoPixelFormat::NV12 ||
              frame->format == VideoPixelFormat::P010) {
            if (frame->stride <= 0 || frame->planeHeight <= 0) {
              renderFailed = true;
              renderFailMessage = "Invalid video frame buffer.";
              renderFailDetail = "Missing YUV plane metadata.";
              return;
            }
            size_t strideBytes = static_cast<size_t>(frame->stride);
            size_t planeHeight = static_cast<size_t>(frame->planeHeight);
            size_t yBytes = strideBytes * planeHeight;
            if (strideBytes == 0 || planeHeight == 0 ||
                yBytes / strideBytes != planeHeight) {
              renderFailed = true;
              renderFailMessage = "Invalid video frame buffer.";
              renderFailDetail = "Invalid YUV plane layout.";
              return;
            }
            size_t required = yBytes + yBytes / 2;
            if (required < yBytes || frame->yuv.size() < required) {
              renderFailed = true;
              renderFailMessage = "Invalid video frame buffer.";
              renderFailDetail = "Frame pixel data is missing.";
              return;
            }

            bool renderedWithGpu = false;
            if (gpuAvailable &&
                (frame->format == VideoPixelFormat::NV12 ||
                 frame->format == VideoPixelFormat::P010)) {
              auto [outW, outH] =
                  computeAsciiOutputSize(width, maxHeight, frame->width,
                                         frame->height);
              art.width = outW;
              art.height = outH;
              std::string gpuErr;
              
              // Stats calculation moved to GPU (StatsCS)
              if (gpuRenderer.RenderNV12(frame->yuv.data(), frame->width,
                                         frame->height, frame->stride,
                                         frame->planeHeight, frame->fullRange, 
                                         frame->yuvMatrix, frame->yuvTransfer,
                                         frame->format == VideoPixelFormat::P010,
                                         art, &gpuErr)) {
                renderedWithGpu = true;
                artOk = true;
                static bool gpuLogged = false;
                if (!gpuLogged) {
                  const char* inputFormat =
                      (frame->format == VideoPixelFormat::P010) ? "p010" : "nv12";
                  perfLogAppendf(&perfLog,
                                 "video_renderer_input format=%s in=%dx%d out=%dx%d",
                                 inputFormat, frame->width, frame->height, outW, outH);
                  appendTiming("video_renderer gpu_active=1 format=nv12 path=cpu_upload");
                  gpuLogged = true;
                }
              } else {
                gpuAvailable = false;
                appendVideoWarning("GPU renderer failed (NV12), falling back to CPU: " +
                                   gpuErr);
                throw std::runtime_error("ASCII-renderer GPU failure" + gpuErr);
              }
            }

            if (!renderedWithGpu) {
              YuvFormat yuvFormat = (frame->format == VideoPixelFormat::P010)
                                        ? YuvFormat::P010
                                        : YuvFormat::NV12;
              artOk = renderAsciiArtFromYuv(
                  frame->yuv.data(), frame->width, frame->height, frame->stride,
                  frame->planeHeight, yuvFormat, frame->fullRange,
                  frame->yuvMatrix, frame->yuvTransfer, width, maxHeight, art);
              static bool cpuLogged = false;
              if (!cpuLogged) {
                appendTiming("video_renderer gpu_active=0 format=nv12 path=cpu_render");
                cpuLogged = true;
              }
            }
          } else {
            size_t expected = static_cast<size_t>(frame->width) *
                              static_cast<size_t>(frame->height) * 4u;
            if (frame->rgba.size() < expected) {
              renderFailed = true;
              renderFailMessage = "Invalid video frame buffer.";
              renderFailDetail = "Frame pixel data is missing.";
              return;
            }

            bool renderedWithGpu = false;
            if (gpuAvailable) {
              auto [outW, outH] =
                  computeAsciiOutputSize(width, maxHeight, frame->width,
                                         frame->height);
              art.width = outW;
              art.height = outH;
              std::string gpuErr;
              if (gpuRenderer.Render(frame->rgba.data(), frame->width,
                                     frame->height, art, &gpuErr)) {
                renderedWithGpu = true;
                artOk = true;
                static bool gpuLogged = false;
                if (!gpuLogged) {
                  appendTiming("video_renderer gpu_active=1 format=rgba path=cpu_upload");
                  gpuLogged = true;
                }
              } else {
                // If GPU fails, disable it for this session and fall back
                gpuAvailable = false;
                appendVideoWarning("GPU renderer failed, falling back to CPU: " +
                                   gpuErr);
              }
            }

            if (!renderedWithGpu) {
              artOk = renderAsciiArtFromRgba(frame->rgba.data(), frame->width,
                                             frame->height, width, maxHeight,
                                             art, true);
              static bool cpuLogged = false;
              if (!cpuLogged) {
                appendTiming("video_renderer gpu_active=0 format=rgba path=cpu_render");
                cpuLogged = true;
              }
            }
          }
        } catch (const std::bad_alloc&) {
          renderFailed = true;
          renderFailMessage = "Failed to render video frame.";
          renderFailDetail = "Out of memory.";
          return;
        } catch (...) {
          renderFailed = true;
          renderFailMessage = "Failed to render video frame.";
          renderFailDetail = "";
          return;
        }
        if (!artOk) {
          renderFailed = true;
          renderFailMessage = "Failed to render video frame.";
          renderFailDetail = "";
          return;
        }
        cachedWidth = width;
        cachedMaxHeight = maxHeight;
        cachedFrameWidth = frame->width;
        cachedFrameHeight = frame->height;
      } else if (!allowFrame) {
        cachedWidth = -1;
        cachedMaxHeight = -1;
        cachedFrameWidth = -1;
        cachedFrameHeight = -1;
      }
    } else {
      if (frame->width <= 0 || frame->height <= 0) {
        renderFailed = true;
        renderFailMessage = "Invalid video frame size.";
        renderFailDetail = "";
        return;
      }
      cachedWidth = width;
      cachedMaxHeight = maxHeight;
      cachedFrameWidth = frame->width;
      cachedFrameHeight = frame->height;
    }

    screen.clear(baseStyle);
    if (!subtitle.empty()) {
      screen.writeText(0, 0, fitLine(subtitle, width), dimStyle);
    }

    if (enableAscii) {
      int artWidth = std::min(art.width, width);
      int artHeight = std::min(art.height, maxHeight);
      int artX = std::max(0, (width - artWidth) / 2);

      if (allowFrame) {
        for (int y = 0; y < artHeight; ++y) {
          for (int x = 0; x < artWidth; ++x) {
            const auto& cell =
                art.cells[static_cast<size_t>(y * art.width + x)];
            Style cellStyle{cell.fg, cell.hasBg ? cell.bg : baseStyle.bg};
            screen.writeChar(artX + x, artTop + y, cell.ch, cellStyle);
          }
        }
      } else {
        screen.writeText(0, artTop, fitLine(waitingLabel(), width), dimStyle);
      }
    } else {
      std::string label = allowFrame
                              ? "ASCII rendering disabled"
                              : waitingLabel();
      screen.writeText(0, artTop, fitLine(label, width), dimStyle);
      if (maxHeight > 1) {
        std::string sizeLine =
            "Video size: " + std::to_string(frame->width) + "x" +
            std::to_string(frame->height);
            screen.writeText(0, artTop + 1, fitLine(sizeLine, width), dimStyle);
      }
    }

    progressBarX = -1;
    progressBarY = -1;
    progressBarWidth = 0;
    if (overlayVisible()) {
      int barLine = height - 1;
      int labelLine = barLine - 1;
      if (labelLine >= artTop && labelLine >= 0) {
        std::string nowLabel = " " + toUtf8String(file.filename());
        screen.writeText(0, labelLine, fitLine(nowLabel, width), accentStyle);
      }

      std::string status;
      bool audioFinished = audioOk && audioIsFinished();
      bool paused = audioOk ? audioIsPaused() : userPaused;
      if (audioFinished) {
        status = "\xE2\x96\xA0";  // ended icon
      } else if (paused) {
        status = "\xE2\x8F\xB8";  // paused icon
      } else {
        status = "\xE2\x96\xB6";  // playing icon
      }
      std::string suffix =
          formatTime(displaySec) + " / " + formatTime(totalSec) + " " + status;
      int suffixWidth = utf8CodepointCount(suffix);
      int barWidth = width - suffixWidth - 3;
      if (barWidth < 10) {
        suffix = formatTime(displaySec) + "/" + formatTime(totalSec);
        suffixWidth = utf8CodepointCount(suffix);
        barWidth = width - suffixWidth - 3;
      }
      if (barWidth < 10) {
        suffix = formatTime(displaySec);
        suffixWidth = utf8CodepointCount(suffix);
        barWidth = width - suffixWidth - 3;
      }
      if (barWidth < 5) {
        suffix.clear();
        barWidth = width - 2;
      }
      int maxBar = std::max(5, width - 2);
      barWidth = std::clamp(barWidth, 5, maxBar);
      double ratio = 0.0;
      if (totalSec > 0.0 && std::isfinite(totalSec)) {
        ratio = std::clamp(displaySec / totalSec, 0.0, 1.0);
      }
      progressBarX = 1;
      progressBarY = barLine;
      progressBarWidth = barWidth;
      screen.writeChar(0, barLine, L'|', progressFrameStyle);
      auto barCells = renderProgressBarCells(ratio, barWidth,
                                             progressEmptyStyle, progressStart,
                                             progressEnd);
      for (int i = 0; i < barWidth; ++i) {
        const auto& cell = barCells[static_cast<size_t>(i)];
        screen.writeChar(1 + i, barLine, cell.ch, cell.style);
      }
      screen.writeChar(1 + barWidth, barLine, L'|', progressFrameStyle);
      if (!suffix.empty()) {
        screen.writeText(2 + barWidth, barLine, " " + suffix, baseStyle);
      }
    }

    screen.draw();
  };

  auto presentNextFrame = [&]() -> bool {
    if (audioStarting) {
      return false;
    }
    if (audioOk && !audioStreamIsClockReady()) {
      return false;
    }
    int pendingSerial = pendingSeekSerial.load();
    if (pendingSerial != 0 && currentSerial.load() != pendingSerial) {
      return false;
    }
    uint64_t serial = static_cast<uint64_t>(currentSerial.load());
    while (true) {
      QueuedFrame front{};
      {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (frameQueue.empty()) return false;
        front = frameQueue.front();
      }

      if (front.serial != serial) {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (!frameQueue.empty() &&
            frameQueue.front().poolIndex == front.poolIndex) {
          freeFrames.push_back(frameQueue.front().poolIndex);
          frameQueue.pop_front();
          queueCv.notify_all();
        }
        lastPresentDropped = 1;
        continue;
      }

      int64_t masterUs = masterClock.get(nowUs());
      if (!audioOk && pendingSerial != 0) {
        masterClock.set(front.ptsUs, nowUs());
        masterUs = front.ptsUs;
      }
      int64_t frameDurUs =
          front.durationUs > 0 ? front.durationUs : lastPresentedDurationUs;
      if (frameDurUs <= 0) {
        frameDurUs = (fallbackFrameDurationUs > 0) ? fallbackFrameDurationUs
                                                   : 33333;
      }
      int64_t syncThresholdUs =
          std::clamp<int64_t>(frameDurUs, 40000, 100000);
      int64_t diff = front.ptsUs - masterUs;

      if (diff < -syncThresholdUs) {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (!frameQueue.empty()) {
          freeFrames.push_back(frameQueue.front().poolIndex);
          frameQueue.pop_front();
          queueCv.notify_all();
        }
        lastPresentDropped = 1;
        continue;
      }
      if (diff > syncThresholdUs) {
        int64_t sleepUs = diff - syncThresholdUs / 2;
        if (sleepUs > 0) {
          int64_t capped = std::min<int64_t>(sleepUs, 5000);
          std::this_thread::sleep_for(std::chrono::microseconds(capped));
        }
        return false;
      }

      QueuedFrame item{};
      {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (frameQueue.empty()) return false;
        item = std::move(frameQueue.front());
        frameQueue.pop_front();
        queueCv.notify_all();
      }
      setCurrentFrame(item.poolIndex);
      lastPresentedPtsUs = item.ptsUs;
      lastPresentedDurationUs =
          item.durationUs > 0 ? item.durationUs : frameDurUs;
      if (pendingSerial != 0 &&
          item.serial == static_cast<uint64_t>(pendingSerial)) {
        pendingSeekSerial.store(0);
      }
      if (!audioOk) {
        masterClock.set(lastPresentedPtsUs, nowUs());
      }
      lastPresentPopped = 1;
      lastPresentDropped = 0;
      return true;
    }
  };

  if (!enableAudio) {
    audioStartOk.store(false);
    audioStartDone.store(true);
    audioStarting = false;
  }

  auto finalizeAudioStart = [&]() {
    if (!audioStarting || !audioStartDone.load()) return;
    audioOk = audioStartOk.load();
    audioStarting = false;
    perfLogAppendf(&perfLog, "audio_start ok=%d", audioOk ? 1 : 0);
    if (audioOk) {
      AudioPerfStats stats = audioGetPerfStats();
      if (stats.periodFrames > 0 && stats.periods > 0) {
        perfLogAppendf(
            &perfLog,
            "audio_device period_frames=%u periods=%u buffer_frames=%u rate=%u "
            "channels=%u using_ffmpeg=%d",
            stats.periodFrames, stats.periods, stats.bufferFrames,
            stats.sampleRate, stats.channels, stats.usingFfmpeg ? 1 : 0);
      }
    }
    redraw = true;
  };

  auto issueSeekRequest = [&](double targetSec) {
    int64_t targetTs =
        firstTs + static_cast<int64_t>(targetSec * secondsToTicks);
    int serial = serialCounter.fetch_add(1) + 1;
    DecodeCommand cmd;
    cmd.seek = true;
    cmd.seekTs = targetTs;
    cmd.serial = serial;
    issueCommand(cmd);
    audioStreamFlush(serial);
    perfLogAppendf(&perfLog,
                   "seek_request serial=%d target_sec=%.3f target_ts=%lld",
                   serial, targetSec, static_cast<long long>(targetTs));
    pendingSeekSerial.store(serial);
    pendingSeekTargetSec = targetSec;
    videoEnded = false;
    forceRefreshArt = true;
    redraw = true;
  };
  renderScreen(true);
  if (renderFailed) {
    if (audioStarting) {
      audioOk = audioStartOk.load();
      audioStarting = false;
    }
    stopPipeline();
    if (audioOk) audioStop();
    bool ok = reportVideoError(renderFailMessage, renderFailDetail);
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }

  while (running) {
    if (audioStarting && audioStartDone.load()) {
      finalizeAudioStart();
    }

    InputEvent ev{};
    while (input.poll(ev)) {
      if (ev.type == InputEvent::Type::Resize) {
        pendingResize = true;
        redraw = true;
        continue;
      }
      if (ev.type == InputEvent::Type::Key) {
        const KeyEvent& key = ev.key;
        const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
        bool ctrl = (key.control & ctrlMask) != 0;
        if ((key.vk == 'C' || key.ch == 'c' || key.ch == 'C') && ctrl) {
          running = false;
          break;
        }
        if (key.vk == VK_ESCAPE || key.vk == 'Q' || key.ch == 'q' ||
            key.ch == 'Q') {
          running = false;
          break;
        }
        if (key.vk == VK_SPACE || key.ch == ' ') {
          if (audioOk) {
            audioTogglePause();
            userPaused = audioIsPaused();
          } else {
            userPaused = !userPaused;
            masterClock.set_paused(userPaused, nowUs());
          }
          redraw = true;
        } else if (key.vk == 'R' || key.ch == 'r' || key.ch == 'R') {
          if (audioOk) audioToggleRadio();
          redraw = true;
        } else if (ctrl && (key.vk == VK_LEFT || key.vk == VK_RIGHT)) {
          triggerOverlay();
          int dir = (key.vk == VK_LEFT) ? -1 : 1;
          double currentSec = currentTimeSec();
          double totalSec = totalDurationSec();
          double targetSec = currentSec + dir * 5.0;
          if (totalSec > 0.0) {
            targetSec = std::clamp(targetSec, 0.0, totalSec);
          } else if (targetSec < 0.0) {
            targetSec = 0.0;
          }
          issueSeekRequest(targetSec);
        }
      }
      if (ev.type == InputEvent::Type::Mouse) {
        triggerOverlay();
        redraw = true;
        const MouseEvent& mouse = ev.mouse;
        bool leftPressed =
            (mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
        if (leftPressed &&
            (mouse.eventFlags == 0 || mouse.eventFlags == MOUSE_MOVED)) {
          if (progressBarWidth > 0 && mouse.pos.Y == progressBarY &&
              progressBarX >= 0) {
            int rel = mouse.pos.X - progressBarX;
            if (rel >= 0 && rel < progressBarWidth) {
              double denom =
                  static_cast<double>(std::max(1, progressBarWidth - 1));
              double ratio = static_cast<double>(rel) / denom;
              ratio = std::clamp(ratio, 0.0, 1.0);
              double totalSec = totalDurationSec();
              if (totalSec > 0.0 && std::isfinite(totalSec)) {
                double targetSec = ratio * totalSec;
                issueSeekRequest(targetSec);
              }
              continue;
            }
          }
        }
      }
    }
    if (!running) break;

    finalizeAudioStart();

    if (scaleWarningPending.exchange(false)) {
      std::string warning = getScaleWarning();
      if (!warning.empty()) {
        appendVideoWarning(warning);
      }
    }

    if (pendingResize) {
      screen.updateSize();
      int width = std::max(20, screen.width());
      int height = std::max(10, screen.height());
      if (allowDecoderScale) {
        requestTargetSize(width, height);
      }
      pendingResize = false;
      redraw = true;
    }

    bool presented = presentNextFrame();
    if (presented) {
      redraw = true;
      forceRefreshArt = true;
    }

    if (!videoEnded && decodeEnded.load()) {
      std::lock_guard<std::mutex> lock(queueMutex);
      if (frameQueue.empty()) {
        videoEnded = true;
      }
    }
    if (videoEnded && (!audioOk || audioIsFinished())) {
      running = false;
    }

    if (redraw || overlayVisible()) {
      renderScreen(presented || forceRefreshArt);
      if (renderFailed) {
        running = false;
        break;
      }
      redraw = false;
      forceRefreshArt = false;
    }

    if (!redraw && !overlayVisible()) {
      std::unique_lock<std::mutex> lock(queueMutex);
      queueCv.wait_for(lock, std::chrono::milliseconds(10), [&]() {
        return !frameQueue.empty() || decodeEnded.load() ||
               commandPendingAtomic.load();
      });
    }
  }

  if (audioStarting) {
    audioOk = audioStartOk.load();
    audioStarting = false;
  }

  if (renderFailed) {
    stopPipeline();
    if (audioOk) audioStop();
    bool ok = reportVideoError(renderFailMessage, renderFailDetail);
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }

  stopPipeline();
  if (audioOk) audioStop();
  finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
  return true;
}
