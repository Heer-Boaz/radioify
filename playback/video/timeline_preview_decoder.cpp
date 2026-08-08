#include "playback/video/timeline_preview_decoder.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <utility>

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

}  // namespace

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

  VideoFrame candidate;
  bool haveCandidate = false;
  bool reachedTarget = false;
  for (int decodedFrames = 0;
       decodedFrames < kMaximumDecodeFrames &&
       !impl_->cancelled(requestId) && !impl_->deadlineExpired();
       ++decodedFrames) {
    VideoFrame frame;
    if (!impl_->decoder.readFrame(frame, nullptr, true)) break;
    const int64_t ptsUs = std::max<int64_t>(0, frame.timestamp100ns / 10);
    int64_t durationUs = frame.duration100ns / 10;
    if (durationUs <= 0) durationUs = kFallbackFrameDurationUs;
    candidate = std::move(frame);
    haveCandidate = true;
    if (ptsUs >= targetUs ||
        (ptsUs <= targetUs && durationUs > targetUs - ptsUs)) {
      reachedTarget = true;
      break;
    }
  }

  const bool cancelled = impl_->cancelled(requestId);
  const bool deadlineExpired = impl_->deadlineExpired();
  impl_->deadlineUs.store(0, std::memory_order_relaxed);
  if (cancelled) {
    impl_->closeDecoder();
    result.status = DecodeStatus::Cancelled;
    return result;
  }
  if (deadlineExpired || !haveCandidate ||
      (!reachedTarget && !impl_->decoder.reachedEndOfStream())) {
    impl_->closeDecoder();
    result.status = DecodeStatus::Failed;
    return result;
  }

  result.status = DecodeStatus::Ready;
  result.frameUs = std::max<int64_t>(0, candidate.timestamp100ns / 10);
  result.frame = std::move(candidate);
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
