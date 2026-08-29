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
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include "core/pumpable_worker_thread.h"
#include "core/waitable_signal.h"
#include "playback/video/image.h"
#include "playback/video/timeline_preview_decoder.h"

namespace playback_video_timeline_preview {

namespace {

constexpr size_t kMaximumPendingPersistentWrites = 32;
constexpr size_t kPersistentPruneWriteInterval = 64;

}  // namespace

struct Provider::Impl {
  struct Work {
    int64_t targetUs = 0;
    uint64_t requestId = 0;
    bool interactive = false;
  };

  explicit Impl(CacheConfig config)
      : cache(config), persistenceCache(std::move(config)) {}

  Source source;
  std::mutex mutex;
  std::condition_variable workAvailable;
  std::condition_variable persistenceAvailable;
  std::optional<Work> demand;
  std::deque<Work> prefetch;
  std::deque<std::shared_ptr<const Image>> pendingPersistence;
  std::optional<Result> result;
  WaitableSignal changed;
  PumpableWorkerThread worker;
  PumpableWorkerThread cacheIo;
  Cache cache;
  Cache persistenceCache;
  Decoder decoder;
  std::atomic<bool> stopping{false};
  std::atomic<uint64_t> latestRequestId{0};
  std::atomic<uint64_t> nextImageId{1};
  bool started = false;
  bool persistenceStarted = false;

  bool isCurrent(uint64_t requestId) const {
    return !stopping.load(std::memory_order_relaxed) && requestId != 0 &&
           requestId == latestRequestId.load(std::memory_order_relaxed);
  }

  std::shared_ptr<const Image> decode(const Work& work) {
    DecodeResult decoded = decoder.decode(work.targetUs, work.requestId);
    if (decoded.status != DecodeStatus::Ready ||
        !playback_video_image::validate(decoded.surface)) {
      return {};
    }
    auto image = std::make_shared<Image>();
    image->id = nextImageId.fetch_add(1, std::memory_order_relaxed);
    image->requestedUs = work.targetUs;
    image->decodedFrameUs = decoded.decodedFrameUs;
    image->surface = std::move(decoded.surface);
    return image;
  }

  void publish(const Work& work, const std::shared_ptr<const Image>& image,
               ResultOrigin origin) {
    if (!work.interactive || !isCurrent(work.requestId)) return;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!isCurrent(work.requestId)) return;
      result = Result{work.requestId, work.targetUs, origin, image};
    }
    changed.signal();
  }

  void persistLater(const std::shared_ptr<const Image>& image) {
    if (!image || !persistenceStarted) return;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (stopping.load(std::memory_order_relaxed)) return;
      const bool alreadyQueued = std::any_of(
          pendingPersistence.begin(), pendingPersistence.end(),
          [&](const std::shared_ptr<const Image>& queued) {
            return queued && queued->requestedUs == image->requestedUs;
          });
      if (alreadyQueued) return;
      if (pendingPersistence.size() >= kMaximumPendingPersistentWrites) {
        pendingPersistence.pop_front();
      }
      pendingPersistence.push_back(image);
    }
    persistenceAvailable.notify_one();
  }

  void runPersistence() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const bool persistentReady = persistenceCache.open(source);
    if (persistentReady) persistenceCache.prunePersistent();
    size_t writesSincePrune = 0;
    for (;;) {
      std::shared_ptr<const Image> image;
      {
        std::unique_lock<std::mutex> lock(mutex);
        persistenceAvailable.wait(lock, [&]() {
          return stopping.load(std::memory_order_relaxed) ||
                 !pendingPersistence.empty();
        });
        if (pendingPersistence.empty()) {
          if (stopping.load(std::memory_order_relaxed)) break;
          continue;
        }
        image = std::move(pendingPersistence.front());
        pendingPersistence.pop_front();
      }
      if (persistentReady) persistenceCache.storePersistent(image);
      if (persistentReady &&
          ++writesSincePrune >= kPersistentPruneWriteInterval) {
        persistenceCache.prunePersistent();
        writesSincePrune = 0;
      }
    }
    if (persistentReady && writesSincePrune > 0) {
      persistenceCache.prunePersistent();
    }
    persistenceCache.close();
    changed.signal();
  }

  void runWorker() {
    const bool cacheReady = cache.open(source);
    const bool decoderReady =
        decoder.configure(source, &stopping, &latestRequestId);

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
        }
      }
      SetThreadPriority(GetCurrentThread(),
                        work.interactive ? THREAD_PRIORITY_NORMAL
                                         : THREAD_PRIORITY_BELOW_NORMAL);
      if (!isCurrent(work.requestId)) continue;

      CacheLookup lookup;
      if (cacheReady) {
        lookup = cache.find(
            work.targetUs,
            nextImageId.fetch_add(1, std::memory_order_relaxed));
      }
      std::shared_ptr<const Image> image = lookup.image;
      ResultOrigin origin = lookup.origin;
      if (!image && decoderReady) {
        image = decode(work);
        origin = ResultOrigin::Decoded;
        if (image && cacheReady) {
          cache.storeMemory(image);
          persistLater(image);
        }
      }
      if (!isCurrent(work.requestId)) continue;
      publish(work, image, origin);
    }
    decoder.reset();
    cache.close();
    changed.signal();
  }

  bool startWorkers() {
    if (cacheIo.start([this]() { runPersistence(); })) {
      persistenceStarted = true;
    } else {
      // Persistent caching is optional; decoding and memory caching stay live.
      persistenceStarted = false;
    }
    if (!worker.start([this]() { runWorker(); })) {
      stopping.store(true, std::memory_order_relaxed);
      persistenceAvailable.notify_all();
      cacheIo.join();
      persistenceStarted = false;
      return false;
    }
    return true;
  }
};

Provider::Provider(CacheConfig cacheConfig)
    : impl_(std::make_unique<Impl>(std::move(cacheConfig))) {}

Provider::~Provider() { stop(); }

bool Provider::start(const Source& source) {
  stop();
  if (source.path.empty() || source.videoStreamIndex < 0 ||
      source.durationUs <= 0 || source.sourceWidth <= 0 ||
      source.sourceHeight <= 0) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->source = source;
    impl_->demand.reset();
    impl_->prefetch.clear();
    impl_->pendingPersistence.clear();
    impl_->result.reset();
    impl_->stopping.store(false, std::memory_order_relaxed);
    impl_->latestRequestId.store(0, std::memory_order_relaxed);
    impl_->started = true;
  }
  if (!impl_->startWorkers()) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->started = false;
    return false;
  }
  return true;
}

void Provider::requestStop() {
  if (!impl_) return;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->started && !impl_->worker.joinable() &&
        !impl_->cacheIo.joinable()) {
      return;
    }
    impl_->stopping.store(true, std::memory_order_relaxed);
    impl_->latestRequestId.fetch_add(1, std::memory_order_relaxed);
    impl_->demand.reset();
    impl_->prefetch.clear();
  }
  impl_->workAvailable.notify_all();
  impl_->persistenceAvailable.notify_all();
  impl_->changed.signal();
}

bool Provider::stopReady() const {
  return !impl_ || (impl_->worker.ready() && impl_->cacheIo.ready());
}

bool Provider::finishStop() {
  if (!impl_ || !stopReady()) return !impl_;
  if (!impl_->worker.finish() || !impl_->cacheIo.finish()) return false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->started = false;
    impl_->result.reset();
    impl_->source = Source{};
    impl_->persistenceStarted = false;
    impl_->pendingPersistence.clear();
  }
  impl_->changed.clear();
  return true;
}

void Provider::stop() {
  if (!impl_) return;
  requestStop();
  impl_->worker.join();
  impl_->cacheIo.join();
  (void)finishStop();
}

bool Provider::submit(const Request& request) {
  if (!impl_ || request.id == 0) return false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->started || impl_->source.durationUs <= 0) return false;
    const uint64_t latest =
        impl_->latestRequestId.load(std::memory_order_relaxed);
    if (request.id <= latest) return false;
    impl_->latestRequestId.store(request.id, std::memory_order_relaxed);
    const int64_t targetUs =
        std::clamp(request.targetUs, int64_t{0}, impl_->source.durationUs - 1);
    impl_->demand = Impl::Work{targetUs, request.id, true};
    impl_->prefetch.clear();
    for (const int64_t requestedPrefetch : request.prefetchTargetsUs) {
      const int64_t prefetchTarget = std::clamp(
          requestedPrefetch, int64_t{0}, impl_->source.durationUs - 1);
      if (prefetchTarget == targetUs) continue;
      const bool duplicate = std::any_of(
          impl_->prefetch.begin(), impl_->prefetch.end(),
          [&](const Impl::Work& work) { return work.targetUs == prefetchTarget; });
      if (!duplicate) {
        impl_->prefetch.push_back(
            Impl::Work{prefetchTarget, request.id, false});
      }
    }
  }
  impl_->workAvailable.notify_one();
  return true;
}

void Provider::cancelBefore(uint64_t requestId) {
  if (!impl_ || requestId == 0) return;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->started) return;
    const uint64_t latest =
        impl_->latestRequestId.load(std::memory_order_relaxed);
    if (requestId <= latest) return;
    impl_->latestRequestId.store(requestId, std::memory_order_relaxed);
    impl_->demand.reset();
    impl_->prefetch.clear();
    if (impl_->result && impl_->result->requestId < requestId) {
      impl_->result.reset();
    }
  }
  impl_->workAvailable.notify_one();
}

std::optional<Result> Provider::takeResult() {
  if (!impl_) return std::nullopt;
  impl_->changed.consume();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::optional<Result> result = std::move(impl_->result);
  impl_->result.reset();
  return result;
}

NativeWaitHandle Provider::changedWaitHandle() const {
  return impl_ ? impl_->changed.nativeWaitHandle() : NativeWaitHandle();
}

std::vector<NativeWaitHandle> Provider::stopWaitHandles() const {
  std::vector<NativeWaitHandle> handles;
  if (!impl_) return handles;
  handles.reserve(2);
  if (!impl_->worker.ready()) {
    const NativeWaitHandle worker = impl_->worker.waitHandle();
    handles.push_back(worker);
  }
  if (!impl_->cacheIo.ready()) {
    const NativeWaitHandle cacheIo = impl_->cacheIo.waitHandle();
    handles.push_back(cacheIo);
  }
  return handles;
}

}  // namespace playback_video_timeline_preview
