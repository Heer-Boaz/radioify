#include "playback/video/timeline_preview_decoder.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>
#include <string>
#include <utility>

#include "playback/video/color_conversion.h"
#include "playback/video/decoder.h"

namespace playback_video_timeline_preview {
namespace {

constexpr int kMaximumDecodeFrames = 2048;
constexpr auto kDecodeDeadline = std::chrono::seconds(5);
constexpr int64_t kFallbackFrameDurationUs = 33333;

int64_t steadyNowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool convertToSurfaceImpl(const VideoFrame& frame,
                          playback_video_image::RgbaImage* surface) {
  if (!surface || frame.width <= 0 || frame.height <= 0 ||
      frame.rotationQuarterTurns != 0 || frame.width > kDecodeMaxWidth ||
      frame.height > kDecodeMaxHeight ||
      static_cast<uint64_t>(frame.width) * 4u >
          (std::numeric_limits<uint32_t>::max)()) {
    return false;
  }
  const uint32_t width = static_cast<uint32_t>(frame.width);
  const uint32_t height = static_cast<uint32_t>(frame.height);
  const uint32_t stride = width * 4u;
  size_t outputBytes = 0;
  if (!playback_video_image::requiredBytes(width, height, stride,
                                           &outputBytes)) {
    return false;
  }
  playback_video_image::RgbaImage converted;
  converted.width = width;
  converted.height = height;
  converted.strideBytes = stride;
  try {
    converted.pixels.resize(outputBytes);
  } catch (const std::bad_alloc&) {
    return false;
  }

  if (frame.format == VideoPixelFormat::RGB32 ||
      frame.format == VideoPixelFormat::ARGB32) {
    const size_t sourceStride = frame.stride > 0
                                    ? static_cast<size_t>(frame.stride)
                                    : static_cast<size_t>(stride);
    const size_t rowBytes = static_cast<size_t>(stride);
    if (sourceStride < rowBytes ||
        static_cast<size_t>(height - 1u) >
            ((std::numeric_limits<size_t>::max)() - rowBytes) /
                sourceStride ||
        frame.rgba.size() <
            static_cast<size_t>(height - 1u) * sourceStride + rowBytes) {
      return false;
    }
    for (uint32_t y = 0; y < height; ++y) {
      const uint8_t* source =
          frame.rgba.data() + static_cast<size_t>(y) * sourceStride;
      uint8_t* destination =
          converted.pixels.data() + static_cast<size_t>(y) * stride;
      std::copy_n(source, rowBytes, destination);
      if (frame.format == VideoPixelFormat::RGB32) {
        for (uint32_t x = 0; x < width; ++x) destination[x * 4u + 3u] = 255;
      }
    }
    *surface = std::move(converted);
    return true;
  }

  if (frame.format != VideoPixelFormat::NV12 &&
      frame.format != VideoPixelFormat::P010) {
    return false;
  }
  const bool p010 = frame.format == VideoPixelFormat::P010;
  const size_t bytesPerSample = p010 ? 2u : 1u;
  if ((frame.width & 1) != 0 || (frame.height & 1) != 0 ||
      frame.stride <= 0 || frame.planeHeight < frame.height ||
      (p010 && (frame.stride & 1) != 0) ||
      static_cast<size_t>(frame.stride) <
          static_cast<size_t>(frame.width) * bytesPerSample) {
    return false;
  }
  const size_t sourceStride = static_cast<size_t>(frame.stride);
  const size_t planeHeight = static_cast<size_t>(frame.planeHeight);
  if (planeHeight > (std::numeric_limits<size_t>::max)() / sourceStride) {
    return false;
  }
  const size_t yBytes = planeHeight * sourceStride;
  const size_t uvRows = static_cast<size_t>(frame.height / 2);
  if (uvRows > ((std::numeric_limits<size_t>::max)() - yBytes) /
                   sourceStride ||
      frame.yuv.size() < yBytes + uvRows * sourceStride) {
    return false;
  }

  const uint8_t* yPlane = frame.yuv.data();
  const uint8_t* uvPlane = yPlane + yBytes;
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* destination =
        converted.pixels.data() + static_cast<size_t>(y) * stride;
    if (p010) {
      const auto* yRow = reinterpret_cast<const uint16_t*>(
          yPlane + static_cast<size_t>(y) * sourceStride);
      const auto* uvRow = reinterpret_cast<const uint16_t*>(
          uvPlane + static_cast<size_t>(y / 2u) * sourceStride);
      for (uint32_t x = 0; x < width; ++x) {
        const size_t uvIndex = static_cast<size_t>(x / 2u) * 2u;
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;
        playback_video_color::yuvToRgb8(
            static_cast<int>(yRow[x] >> 6),
            static_cast<int>(uvRow[uvIndex] >> 6),
            static_cast<int>(uvRow[uvIndex + 1u] >> 6), frame.fullRange, 10,
            frame.yuvMatrix, frame.yuvTransfer, red, green, blue);
        destination[x * 4u + 0u] = red;
        destination[x * 4u + 1u] = green;
        destination[x * 4u + 2u] = blue;
        destination[x * 4u + 3u] = 255;
      }
    } else {
      const uint8_t* yRow =
          yPlane + static_cast<size_t>(y) * sourceStride;
      const uint8_t* uvRow =
          uvPlane + static_cast<size_t>(y / 2u) * sourceStride;
      for (uint32_t x = 0; x < width; ++x) {
        const size_t uvIndex = static_cast<size_t>(x / 2u) * 2u;
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;
        playback_video_color::yuvToRgb8(
            yRow[x], uvRow[uvIndex], uvRow[uvIndex + 1u], frame.fullRange, 8,
            frame.yuvMatrix, frame.yuvTransfer, red, green, blue);
        destination[x * 4u + 0u] = red;
        destination[x * 4u + 1u] = green;
        destination[x * 4u + 2u] = blue;
        destination[x * 4u + 3u] = 255;
      }
    }
  }
  *surface = std::move(converted);
  return true;
}

}  // namespace

bool convertFrameToRgba(const VideoFrame& frame,
                        playback_video_image::RgbaImage* surface) {
  return convertToSurfaceImpl(frame, surface);
}

struct Decoder::Impl {
  Source source;
  std::atomic<bool>* stopping = nullptr;
  std::atomic<uint64_t>* latestRequestId = nullptr;
  std::atomic<uint64_t> activeRequestId{0};
  std::atomic<int64_t> deadlineUs{0};
  VideoDecoder decoder;
  bool ready = false;

  static int interrupt(void* opaque) {
    auto* self = static_cast<Impl*>(opaque);
    if (!self || !self->stopping || !self->latestRequestId) return 1;
    if (self->stopping->load(std::memory_order_relaxed)) return 1;
    if (self->activeRequestId.load(std::memory_order_relaxed) !=
        self->latestRequestId->load(std::memory_order_relaxed)) {
      return 1;
    }
    const int64_t deadline = self->deadlineUs.load(std::memory_order_relaxed);
    return deadline > 0 && steadyNowUs() >= deadline ? 1 : 0;
  }

  bool cancelled(uint64_t requestId) const {
    return !stopping || !latestRequestId ||
           stopping->load(std::memory_order_relaxed) ||
           requestId != latestRequestId->load(std::memory_order_relaxed);
  }

  bool deadlineExpired() const {
    const int64_t deadline = deadlineUs.load(std::memory_order_relaxed);
    return deadline > 0 && steadyNowUs() >= deadline;
  }

  void closeDecoder() {
    decoder.uninit();
    ready = false;
  }

  bool ensureDecoder() {
    if (ready) return true;
    std::string error;
    ready = decoder.init(source.path, &error, false, true, nullptr,
                         source.videoStreamIndex, &Impl::interrupt, this);
    if (!ready) return false;
    const auto size = fitDecodeSize(source.sourceWidth, source.sourceHeight);
    if (!decoder.setTargetSize(size.first, size.second, &error)) {
      closeDecoder();
    }
    return ready;
  }
};

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}

Decoder::~Decoder() = default;

bool Decoder::configure(const Source& source, std::atomic<bool>* stopping,
                        std::atomic<uint64_t>* latestRequestId) {
  reset();
  if (source.path.empty() || source.videoStreamIndex < 0 ||
      source.durationUs <= 0 || source.sourceWidth <= 0 ||
      source.sourceHeight <= 0 || !stopping || !latestRequestId) {
    return false;
  }
  impl_->source = source;
  impl_->stopping = stopping;
  impl_->latestRequestId = latestRequestId;
  return true;
}

DecodeResult Decoder::decode(int64_t targetUs, uint64_t requestId) {
  DecodeResult result;
  if (requestId == 0 || impl_->source.durationUs <= 0) return result;
  targetUs = std::clamp(targetUs, int64_t{0}, impl_->source.durationUs - 1);
  impl_->activeRequestId.store(requestId, std::memory_order_relaxed);
  impl_->deadlineUs.store(
      steadyNowUs() +
          std::chrono::duration_cast<std::chrono::microseconds>(
              kDecodeDeadline)
              .count(),
      std::memory_order_relaxed);

  if (impl_->cancelled(requestId)) {
    impl_->deadlineUs.store(0, std::memory_order_relaxed);
    result.status = DecodeStatus::Cancelled;
    return result;
  }
  if (!impl_->ensureDecoder()) {
    const bool cancelled = impl_->cancelled(requestId);
    impl_->closeDecoder();
    impl_->deadlineUs.store(0, std::memory_order_relaxed);
    result.status = cancelled ? DecodeStatus::Cancelled : DecodeStatus::Failed;
    return result;
  }
  if (targetUs > (std::numeric_limits<int64_t>::max)() / 10 ||
      !impl_->decoder.seekToTimestamp100ns(targetUs * 10)) {
    const bool cancelled = impl_->cancelled(requestId);
    impl_->closeDecoder();
    impl_->deadlineUs.store(0, std::memory_order_relaxed);
    result.status = cancelled ? DecodeStatus::Cancelled : DecodeStatus::Failed;
    return result;
  }

  bool haveCandidate = false;
  bool reachedTarget = false;
  for (int decodedFrames = 0;
       decodedFrames < kMaximumDecodeFrames &&
       !impl_->cancelled(requestId) && !impl_->deadlineExpired();
       ++decodedFrames) {
    VideoFrame frame;
    // Preroll only needs timing. Pixel transfer/scaling is deferred until the
    // authoritative frame has been selected.
    if (!impl_->decoder.readFrame(frame, nullptr, false)) break;
    const int64_t ptsUs = std::max<int64_t>(0, frame.timestamp100ns / 10);
    int64_t durationUs = frame.duration100ns / 10;
    if (durationUs <= 0) durationUs = kFallbackFrameDurationUs;
    haveCandidate = true;
    if (ptsUs >= targetUs ||
        (ptsUs <= targetUs && durationUs > targetUs - ptsUs)) {
      reachedTarget = true;
      break;
    }
  }

  const bool cancelled = impl_->cancelled(requestId);
  const bool deadlineExpired = impl_->deadlineExpired();
  if (cancelled) {
    impl_->closeDecoder();
    impl_->deadlineUs.store(0, std::memory_order_relaxed);
    result.status = DecodeStatus::Cancelled;
    return result;
  }
  if (deadlineExpired || !haveCandidate ||
      (!reachedTarget && !impl_->decoder.reachedEndOfStream())) {
    impl_->closeDecoder();
    impl_->deadlineUs.store(0, std::memory_order_relaxed);
    result.status = DecodeStatus::Failed;
    return result;
  }

  VideoFrame selected;
  if (!impl_->decoder.redecodeLastFrame(selected) ||
      !convertFrameToRgba(selected, &result.surface) ||
      impl_->cancelled(requestId) || impl_->deadlineExpired()) {
    const bool wasCancelled = impl_->cancelled(requestId);
    impl_->closeDecoder();
    impl_->deadlineUs.store(0, std::memory_order_relaxed);
    result.status = wasCancelled ? DecodeStatus::Cancelled
                                 : DecodeStatus::Failed;
    return result;
  }

  impl_->deadlineUs.store(0, std::memory_order_relaxed);
  result.status = DecodeStatus::Ready;
  result.decodedFrameUs =
      std::max<int64_t>(0, selected.timestamp100ns / 10);
  return result;
}

void Decoder::reset() {
  impl_->deadlineUs.store(0, std::memory_order_relaxed);
  impl_->activeRequestId.store(0, std::memory_order_relaxed);
  impl_->closeDecoder();
  impl_->source = Source{};
  impl_->stopping = nullptr;
  impl_->latestRequestId = nullptr;
}

}  // namespace playback_video_timeline_preview
