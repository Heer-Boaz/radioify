#include "playback/video/timeline_preview.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "core/waitable_signal.h"

namespace playback_video_timeline_preview {
namespace {

constexpr int64_t kDefaultBucketUs = 250000;
constexpr int kInitialStoryboardCount = 9;
constexpr int kMaximumDecodeFrames = 2048;
constexpr auto kDecodeDeadline = std::chrono::seconds(5);
constexpr int64_t kFallbackFrameDurationUs = 33333;

int64_t steadyNowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t saturatedAbsDifference(int64_t a, int64_t b) {
  if (a >= b) return a - b;
  return b - a;
}

}  // namespace

struct Service::Impl {
  struct Work {
    int64_t targetUs = 0;
    int64_t bucketUs = kDefaultBucketUs;
    uint64_t generation = 0;
    bool interactive = false;
    int direction = 0;
  };

  struct CacheEntry {
    std::shared_ptr<const Image> image;
    size_t bytes = 0;
    std::list<int64_t>::iterator lru;
  };

  std::filesystem::path path;
  int videoStreamIndex = -1;
  int64_t durationUs = 0;
  int sourceWidth = 0;
  int sourceHeight = 0;

  mutable std::mutex mutex;
  std::condition_variable workAvailable;
  std::optional<Work> demand;
  std::deque<Work> prefetch;
  std::map<int64_t, CacheEntry> cache;
  std::list<int64_t> lru;
  size_t cacheBytes = 0;
  int64_t lastRequestedDecodeTarget = -1;
  Snapshot currentSnapshot;
  WaitableSignal changed;
  std::thread worker;

  std::atomic<bool> stopping{false};
  std::atomic<uint64_t> latestGeneration{0};
  std::atomic<uint64_t> activeGeneration{0};
  std::atomic<int64_t> activeDeadlineUs{0};
  std::atomic<uint64_t> nextImageId{1};
  bool started = false;

  VideoDecoder decoder;
  bool decoderReady = false;

  static int interruptDecoder(void* opaque) {
    auto* self = static_cast<Impl*>(opaque);
    if (!self) return 1;
    if (self->stopping.load(std::memory_order_relaxed)) return 1;
    const uint64_t active =
        self->activeGeneration.load(std::memory_order_relaxed);
    if (active != self->latestGeneration.load(std::memory_order_relaxed)) {
      return 1;
    }
    const int64_t deadline =
        self->activeDeadlineUs.load(std::memory_order_relaxed);
    return deadline > 0 && steadyNowUs() >= deadline ? 1 : 0;
  }

  bool isCurrent(uint64_t generation) const {
    return !stopping.load(std::memory_order_relaxed) &&
           generation == latestGeneration.load(std::memory_order_relaxed);
  }

  void markChangedLocked() {
    ++currentSnapshot.revision;
  }

  void signalChanged() { changed.signal(); }

  std::shared_ptr<const Image> findCachedLocked(int64_t targetUs,
                                                int64_t toleranceUs) {
    if (cache.empty()) return {};
    auto best = cache.lower_bound(targetUs);
    if (best == cache.end() ||
        (best != cache.begin() &&
         saturatedAbsDifference(std::prev(best)->first, targetUs) <=
             saturatedAbsDifference(best->first, targetUs))) {
      --best;
    }
    if (saturatedAbsDifference(best->first, targetUs) > toleranceUs) {
      return {};
    }
    lru.erase(best->second.lru);
    lru.push_front(best->first);
    best->second.lru = lru.begin();
    return best->second.image;
  }

  void queuePrefetchLocked(int64_t targetUs, int64_t bucketUs,
                           uint64_t generation, bool prioritize = false) {
    if (durationUs <= 0) return;
    targetUs = std::clamp(targetUs, int64_t{0}, durationUs - 1);
    if (cache.find(targetUs) != cache.end() ||
        (demand && demand->targetUs == targetUs)) {
      return;
    }
    const auto pending =
        std::find_if(prefetch.begin(), prefetch.end(), [&](const Work& work) {
          return work.targetUs == targetUs;
        });
    if (pending != prefetch.end()) {
      if (prioritize && pending != prefetch.begin()) {
        Work promoted = *pending;
        prefetch.erase(pending);
        prefetch.push_front(std::move(promoted));
      }
      return;
    }
    Work work{targetUs, bucketUs, generation, false, 0};
    if (prioritize) {
      prefetch.push_front(std::move(work));
    } else {
      prefetch.push_back(std::move(work));
    }
  }

  void queueNeighborsLocked(int64_t targetUs, int64_t bucketUs,
                            uint64_t generation, int direction) {
    const auto queueOffset = [&](int sign, int distance) {
      const int64_t boundedDistance =
          bucketUs > (std::numeric_limits<int64_t>::max)() /
                         std::max(1, distance)
              ? (std::numeric_limits<int64_t>::max)()
              : bucketUs * std::max(1, distance);
      if (sign < 0) {
        queuePrefetchLocked(std::max<int64_t>(0, targetUs - boundedDistance),
                            bucketUs, generation, true);
      } else if (durationUs > 0) {
        const int64_t lastUs = durationUs - 1;
        queuePrefetchLocked(
            targetUs > lastUs - std::min(lastUs, boundedDistance)
                ? lastUs
                : targetUs + boundedDistance,
            bucketUs, generation, true);
      }
    };
    // queueOffset promotes at the front, so enqueue in reverse priority.
    // The immediate frame in the current scrub direction must always win,
    // including while scrubbing backward through long-GOP media.
    if (direction != 0) {
      queueOffset(-direction, 1);
      queueOffset(direction, 2);
      queueOffset(direction, 1);
    } else {
      queueOffset(-1, 1);
      queueOffset(1, 1);
    }
  }

  void insertCacheLocked(const std::shared_ptr<const Image>& image) {
    if (!image || image->frame.storageBytes == 0 ||
        image->frame.storageBytes > kMaxCacheBytes) {
      return;
    }
    auto existing = cache.find(image->requestedUs);
    if (existing != cache.end()) {
      cacheBytes -= existing->second.bytes;
      lru.erase(existing->second.lru);
      cache.erase(existing);
    }
    lru.push_front(image->requestedUs);
    cache.emplace(image->requestedUs,
                  CacheEntry{image, image->frame.storageBytes, lru.begin()});
    cacheBytes += image->frame.storageBytes;
    while ((!lru.empty()) &&
           (cacheBytes > kMaxCacheBytes || cache.size() > kMaxCacheFrames)) {
      const int64_t oldest = lru.back();
      auto it = cache.find(oldest);
      if (it != cache.end()) {
        cacheBytes -= it->second.bytes;
        cache.erase(it);
      }
      lru.pop_back();
    }
  }

  bool ensureDecoder() {
    if (decoderReady) return true;
    std::string error;
    decoderReady = decoder.init(path, &error, false, true, nullptr,
                                videoStreamIndex, &Impl::interruptDecoder, this);
    if (!decoderReady) return false;
    const auto targetSize = fitDecodeSize(sourceWidth > 0 ? sourceWidth
                                                          : decoder.width(),
                                          sourceHeight > 0 ? sourceHeight
                                                           : decoder.height());
    if (!decoder.setTargetSize(targetSize.first, targetSize.second, &error)) {
      decoder.uninit();
      decoderReady = false;
    }
    return decoderReady;
  }

  std::shared_ptr<const Image> decode(const Work& work) {
    activeGeneration.store(work.generation, std::memory_order_relaxed);
    activeDeadlineUs.store(
        steadyNowUs() +
            std::chrono::duration_cast<std::chrono::microseconds>(
                kDecodeDeadline)
                .count(),
        std::memory_order_relaxed);

    if (!ensureDecoder() || !isCurrent(work.generation)) {
      decoder.uninit();
      decoderReady = false;
      return {};
    }
    if (work.targetUs > (std::numeric_limits<int64_t>::max)() / 10 ||
        !decoder.seekToTimestamp100ns(work.targetUs * 10)) {
      decoder.uninit();
      decoderReady = false;
      return {};
    }

    VideoFrame candidate;
    bool haveCandidate = false;
    for (int decodedFrames = 0;
         decodedFrames < kMaximumDecodeFrames && isCurrent(work.generation);
         ++decodedFrames) {
      VideoFrame frame;
      if (!decoder.readFrame(frame, nullptr, true)) break;
      const int64_t ptsUs = std::max<int64_t>(0, frame.timestamp100ns / 10);
      int64_t frameDurationUs = frame.duration100ns / 10;
      if (frameDurationUs <= 0) frameDurationUs = kFallbackFrameDurationUs;
      candidate = std::move(frame);
      haveCandidate = true;
      if (ptsUs >= work.targetUs ||
          (ptsUs <= work.targetUs &&
           frameDurationUs > work.targetUs - ptsUs)) {
        break;
      }
    }

    const bool deadlineExpired =
        activeDeadlineUs.load(std::memory_order_relaxed) > 0 &&
        steadyNowUs() >= activeDeadlineUs.load(std::memory_order_relaxed);
    activeDeadlineUs.store(0, std::memory_order_relaxed);
    if (!isCurrent(work.generation) || deadlineExpired) {
      decoder.uninit();
      decoderReady = false;
      return {};
    }
    if (!haveCandidate) {
      if (decoder.atEnd()) {
        decoder.uninit();
        decoderReady = false;
      }
      return {};
    }

    auto image = std::make_shared<Image>();
    image->id = nextImageId.fetch_add(1, std::memory_order_relaxed);
    image->requestedUs = work.targetUs;
    image->frameUs = std::max<int64_t>(0, candidate.timestamp100ns / 10);
    image->frame = std::move(candidate);
    return image;
  }

  void runWorker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (!stopping.load(std::memory_order_relaxed)) {
      Work work;
      {
        std::unique_lock<std::mutex> lock(mutex);
        workAvailable.wait(lock, [&]() {
          return stopping.load(std::memory_order_relaxed) || demand ||
                 !prefetch.empty();
        });
        if (stopping.load(std::memory_order_relaxed)) break;
        if (demand) {
          work = *demand;
          demand.reset();
        } else {
          work = prefetch.front();
          prefetch.pop_front();
          // A prefetch queued before the newest hover request adopts the newest
          // cancellation generation when it begins.
          work.generation =
              latestGeneration.load(std::memory_order_relaxed);
        }
      }

      std::shared_ptr<const Image> image = decode(work);
      bool notify = false;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (image) insertCacheLocked(image);
        if (work.interactive && isCurrent(work.generation) &&
            currentSnapshot.visible &&
            currentSnapshot.targetUs >= 0) {
          const int64_t currentBucketTarget = bucketTargetUs(
              currentSnapshot.targetUs, durationUs, work.bucketUs);
          if (currentBucketTarget == work.targetUs) {
            currentSnapshot.image = image;
            currentSnapshot.loading = false;
            currentSnapshot.failed = !image;
            markChangedLocked();
            notify = true;
            if (image) {
              queueNeighborsLocked(work.targetUs, work.bucketUs,
                                   work.generation, work.direction);
            }
          }
        }
      }
      if (notify) signalChanged();
    }
    decoder.uninit();
    decoderReady = false;
  }

  bool startWorker() {
    try {
      worker = std::thread([this]() { runWorker(); });
    } catch (...) {
      return false;
    }
    return true;
  }
};

Service::Service() : impl_(std::make_unique<Impl>()) {}

Service::~Service() { stop(); }

bool Service::start(const std::filesystem::path& path, int videoStreamIndex,
                    int64_t durationUs, int sourceWidth, int sourceHeight) {
  stop();
  if (path.empty() || videoStreamIndex < 0 || durationUs <= 0 ||
      sourceWidth <= 0 || sourceHeight <= 0) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->path = path;
    impl_->videoStreamIndex = videoStreamIndex;
    impl_->durationUs = durationUs;
    impl_->sourceWidth = sourceWidth;
    impl_->sourceHeight = sourceHeight;
    impl_->currentSnapshot = Snapshot{};
    impl_->currentSnapshot.durationUs = durationUs;
    impl_->demand.reset();
    impl_->prefetch.clear();
    impl_->cache.clear();
    impl_->lru.clear();
    impl_->cacheBytes = 0;
    impl_->lastRequestedDecodeTarget = -1;
    impl_->stopping.store(false, std::memory_order_relaxed);
    impl_->latestGeneration.store(1, std::memory_order_relaxed);
    impl_->started = true;

    const int64_t warmBucket = bucketDurationUs(durationUs, 96);
    for (int i = 0; i < kInitialStoryboardCount; ++i) {
      const double ratio = static_cast<double>(i) /
                           static_cast<double>(kInitialStoryboardCount - 1);
      const int64_t rawTarget = static_cast<int64_t>(
          std::llround(ratio * static_cast<double>(durationUs - 1)));
      impl_->queuePrefetchLocked(
          bucketTargetUs(rawTarget, durationUs, warmBucket), warmBucket, 1);
    }
  }
  if (!impl_->startWorker()) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->started = false;
    impl_->prefetch.clear();
    return false;
  }
  impl_->workAvailable.notify_one();
  return true;
}

void Service::stop() {
  if (!impl_) return;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->started && !impl_->worker.joinable()) return;
    impl_->stopping.store(true, std::memory_order_relaxed);
    impl_->latestGeneration.fetch_add(1, std::memory_order_relaxed);
    impl_->demand.reset();
    impl_->prefetch.clear();
  }
  impl_->workAvailable.notify_all();
  if (impl_->worker.joinable()) impl_->worker.join();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->started = false;
    impl_->currentSnapshot = Snapshot{};
    impl_->cache.clear();
    impl_->lru.clear();
    impl_->cacheBytes = 0;
  }
  impl_->changed.clear();
}

bool Service::request(double ratio, int progressUnits) {
  if (!impl_ || !std::isfinite(ratio)) return false;
  bool notifyWorker = false;
  bool notifyUi = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->started || impl_->durationUs <= 0) return false;
    ratio = std::clamp(ratio, 0.0, 1.0);
    const int64_t targetUs = static_cast<int64_t>(std::llround(
        ratio * static_cast<double>(impl_->durationUs - 1)));
    const int64_t bucketUs =
        bucketDurationUs(impl_->durationUs, progressUnits);
    const int64_t decodeTargetUs =
        bucketTargetUs(targetUs, impl_->durationUs, bucketUs);
    const bool sameDecodeTarget =
        impl_->currentSnapshot.visible &&
        bucketTargetUs(impl_->currentSnapshot.targetUs, impl_->durationUs,
                       bucketUs) == decodeTargetUs;
    const int direction =
        impl_->lastRequestedDecodeTarget < 0
            ? 0
            : (decodeTargetUs > impl_->lastRequestedDecodeTarget
                   ? 1
                   : (decodeTargetUs < impl_->lastRequestedDecodeTarget ? -1
                                                                         : 0));
    impl_->lastRequestedDecodeTarget = decodeTargetUs;
    uint64_t generation =
        impl_->latestGeneration.load(std::memory_order_relaxed);
    if (!sameDecodeTarget) {
      generation =
          impl_->latestGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    impl_->currentSnapshot.visible = true;
    impl_->currentSnapshot.anchorRatio = ratio;
    impl_->currentSnapshot.targetUs = targetUs;
    impl_->currentSnapshot.durationUs = impl_->durationUs;
    impl_->currentSnapshot.failed = false;

    std::shared_ptr<const Image> cached = impl_->findCachedLocked(
        decodeTargetUs, std::max<int64_t>(1, bucketUs / 2));
    if (cached) {
      impl_->currentSnapshot.image = std::move(cached);
      impl_->currentSnapshot.loading = false;
      impl_->demand.reset();
      impl_->queueNeighborsLocked(decodeTargetUs, bucketUs, generation,
                                  direction);
      notifyWorker = true;
    } else if (!sameDecodeTarget || !impl_->currentSnapshot.loading) {
      if (sameDecodeTarget) {
        generation =
            impl_->latestGeneration.fetch_add(1, std::memory_order_relaxed) +
            1;
      }
      impl_->currentSnapshot.image.reset();
      impl_->currentSnapshot.loading = true;
      impl_->demand =
          Impl::Work{decodeTargetUs, bucketUs, generation, true, direction};
      notifyWorker = true;
    }
    impl_->markChangedLocked();
    notifyUi = true;
  }
  if (notifyWorker) impl_->workAvailable.notify_one();
  if (notifyUi) impl_->signalChanged();
  return true;
}

bool Service::hide() {
  if (!impl_) return false;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->currentSnapshot.visible) return false;
    impl_->currentSnapshot.visible = false;
    impl_->currentSnapshot.loading = false;
    impl_->currentSnapshot.failed = false;
    impl_->currentSnapshot.image.reset();
    impl_->demand.reset();
    impl_->latestGeneration.fetch_add(1, std::memory_order_relaxed);
    impl_->markChangedLocked();
    changed = true;
  }
  impl_->workAvailable.notify_one();
  if (changed) impl_->signalChanged();
  return changed;
}

Snapshot Service::snapshot() const {
  if (!impl_) return {};
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->currentSnapshot;
}

bool Service::consumeChanged() {
  return impl_ && impl_->changed.consume();
}

NativeWaitHandle Service::changedWaitHandle() const {
  return impl_ ? impl_->changed.nativeWaitHandle() : NativeWaitHandle();
}

}  // namespace playback_video_timeline_preview
