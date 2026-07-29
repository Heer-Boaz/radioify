#include "playback/video/frame_step_prefetch.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <new>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

#include <wrl/client.h>

namespace playback_video_frame_step_prefetch {
namespace {

constexpr int64_t kFallbackFrameDurationUs = 33333;

int64_t frameEndUs(int64_t ptsUs, int64_t durationUs) {
  if (durationUs <= 0 ||
      ptsUs > (std::numeric_limits<int64_t>::max)() - durationUs) {
    return ptsUs;
  }
  return ptsUs + durationUs;
}

bool translateFromAnchor(int64_t value, int64_t fromAnchor, int64_t toAnchor,
                         int64_t* translated) {
  if (!translated || value < 0 || fromAnchor < 0 || toAnchor < 0) {
    return false;
  }
  if (value >= fromAnchor) {
    const int64_t delta = value - fromAnchor;
    if (toAnchor > (std::numeric_limits<int64_t>::max)() - delta) {
      return false;
    }
    *translated = toAnchor + delta;
    return true;
  }

  const int64_t delta = fromAnchor - value;
  *translated = toAnchor >= delta ? toAnchor - delta : int64_t{0};
  return true;
}

bool microsecondsTo100ns(int64_t valueUs, int64_t* value100ns) {
  if (!value100ns || valueUs < 0 ||
      valueUs > (std::numeric_limits<int64_t>::max)() / 10) {
    return false;
  }
  *value100ns = valueUs * 10;
  return true;
}

class CacheBudget : public std::enable_shared_from_this<CacheBudget> {
 public:
  std::shared_ptr<void> acquire(size_t bytes) {
    if (bytes == 0 || bytes > kMaxCachedBytes) {
      return {};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (frameCount_ >= kMaxCachedFrameCount ||
        bytes_ > kMaxCachedBytes - bytes) {
      return {};
    }
    ++frameCount_;
    bytes_ += bytes;
    try {
      return std::static_pointer_cast<void>(
          std::make_shared<Lease>(shared_from_this(), bytes));
    } catch (const std::bad_alloc&) {
      --frameCount_;
      bytes_ -= bytes;
      return {};
    }
  }

 private:
  struct Lease {
    Lease(std::shared_ptr<CacheBudget> ownerIn, size_t bytesIn)
        : owner(std::move(ownerIn)), bytes(bytesIn) {}
    ~Lease() { owner->release(bytes); }

    std::shared_ptr<CacheBudget> owner;
    size_t bytes = 0;
  };

  void release(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    assert(frameCount_ > 0);
    assert(bytes_ >= bytes);
    --frameCount_;
    bytes_ -= bytes;
  }

 public:
  static std::shared_ptr<CacheBudget> create() {
    return std::make_shared<CacheBudget>();
  }

 private:
  std::mutex mutex_;
  size_t bytes_ = 0;
  size_t frameCount_ = 0;
};

enum class SnapshotResult {
  Ok,
  BudgetFull,
  Failed,
};

SnapshotResult snapshotFrame(
    const VideoFrame& decoded, const std::shared_ptr<CacheBudget>& budget,
    ID3D11Device* device, ID3D11DeviceContext* context,
    std::recursive_mutex* contextMutex, VideoFrame* out) {
  if (!out || !budget) {
    return SnapshotResult::Failed;
  }

  if (decoded.format != VideoPixelFormat::HWTexture) {
    std::shared_ptr<void> lease = budget->acquire(decoded.storageBytes);
    if (!lease) {
      return SnapshotResult::BudgetFull;
    }
    *out = decoded;
    out->hwFrameRef.reset();
    out->cacheLease = std::move(lease);
    return SnapshotResult::Ok;
  }

  if (!decoded.hwTexture || !device || !context || !contextMutex) {
    return SnapshotResult::Failed;
  }

  D3D11_TEXTURE2D_DESC sourceDesc{};
  decoded.hwTexture->GetDesc(&sourceDesc);
  if (sourceDesc.MipLevels != 1 || decoded.hwTextureArrayIndex < 0 ||
      static_cast<UINT>(decoded.hwTextureArrayIndex) >= sourceDesc.ArraySize) {
    return SnapshotResult::Failed;
  }
  std::shared_ptr<void> lease = budget->acquire(decoded.storageBytes);
  if (!lease) {
    return SnapshotResult::BudgetFull;
  }

  D3D11_TEXTURE2D_DESC snapshotDesc = sourceDesc;
  snapshotDesc.MipLevels = 1;
  snapshotDesc.ArraySize = 1;
  snapshotDesc.SampleDesc.Count = 1;
  snapshotDesc.SampleDesc.Quality = 0;
  snapshotDesc.Usage = D3D11_USAGE_DEFAULT;
  // The snapshot is a copy source, not a directly bound shader resource.
  // BindFlags=0 also keeps it valid as a D3D11 video-processor input.
  snapshotDesc.BindFlags = 0;
  snapshotDesc.CPUAccessFlags = 0;
  snapshotDesc.MiscFlags = 0;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> snapshot;
  {
    std::lock_guard<std::recursive_mutex> lock(*contextMutex);
    if (FAILED(
            device->CreateTexture2D(&snapshotDesc, nullptr, &snapshot))) {
      return SnapshotResult::Failed;
    }
    // FFmpeg represents a D3D11VA frame as one array subresource. A single
    // region copy therefore snapshots the complete NV12/P010 surface.
    context->CopySubresourceRegion(
        snapshot.Get(), 0, 0, 0, 0, decoded.hwTexture.Get(),
        static_cast<UINT>(decoded.hwTextureArrayIndex), nullptr);
  }

  *out = decoded;
  out->hwTexture = std::move(snapshot);
  out->hwTextureArrayIndex = 0;
  out->hwFrameRef.reset();
  out->cacheLease = std::move(lease);
  return SnapshotResult::Ok;
}

}  // namespace

struct Prefetcher::Impl {
  struct Work {
    Request request;
    uint64_t generation = 0;
  };

  std::filesystem::path path;
  int videoStreamIndex = -1;
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  std::recursive_mutex* contextMutex = nullptr;
  std::shared_ptr<CacheBudget> budget;
  VideoDecoder decoder;
  bool decoderReady = false;

  mutable std::mutex mutex;
  std::condition_variable cv;
  std::optional<Work> queued;
  std::optional<Work> failed;
  std::vector<Batch> batches;
  std::thread worker;
  bool started = false;
  bool stopping = false;
  bool working = false;
  Work workingRequest;
  uint64_t generation = 0;

  bool current(uint64_t candidate) const {
    std::lock_guard<std::mutex> lock(mutex);
    return !stopping && candidate == generation;
  }

  bool ensureDecoder() {
    if (decoderReady) {
      return true;
    }
    std::string error;
    decoderReady = decoder.initWithDevice(path, device.Get(), &error, nullptr,
                                          contextMutex, videoStreamIndex);
    return decoderReady;
  }

  bool publish(Batch batch, uint64_t candidateGeneration) {
    std::lock_guard<std::mutex> lock(mutex);
    if (stopping || candidateGeneration != generation) {
      return false;
    }
    batches.push_back(std::move(batch));
    cv.notify_all();
    return true;
  }

  SnapshotResult retain(const VideoFrame& decoded, const VideoReadInfo& info,
                        int64_t ptsUs, int64_t durationUs, double decodeMs,
                        CachedFrame* out) {
    if (!out) {
      return SnapshotResult::Failed;
    }
    CachedFrame retained;
    SnapshotResult result =
        snapshotFrame(decoded, budget, device.Get(), context.Get(),
                      contextMutex, &retained.frame);
    if (result != SnapshotResult::Ok) {
      return result;
    }
    retained.info = info;
    retained.ptsUs = ptsUs;
    retained.durationUs = durationUs;
    retained.decodeMs = decodeMs;
    *out = std::move(retained);
    return SnapshotResult::Ok;
  }

  bool retainBeforeFrame(const VideoFrame& decoded, const VideoReadInfo& info,
                         int64_t ptsUs, int64_t durationUs, double decodeMs,
                         std::vector<CachedFrame>* frames) {
    if (!frames) {
      return false;
    }
    CachedFrame retained;
    SnapshotResult result =
        retain(decoded, info, ptsUs, durationUs, decodeMs, &retained);
    while (result == SnapshotResult::BudgetFull && !frames->empty()) {
      frames->erase(frames->begin());
      result = retain(decoded, info, ptsUs, durationUs, decodeMs, &retained);
    }
    if (result != SnapshotResult::Ok) {
      return false;
    }
    frames->push_back(std::move(retained));
    return true;
  }

  bool process(const Work& work) {
    if (!ensureDecoder() || !current(work.generation)) {
      return false;
    }

    const Request& request = work.request;
    constexpr int64_t kUnknown = (std::numeric_limits<int64_t>::min)();
    const int64_t sourceBoundaryUs =
        request.boundary.sourcePtsUs != kUnknown
            ? request.boundary.sourcePtsUs
            : request.boundary.ptsUs;
    int64_t sourceRangeStartUs = 0;
    int64_t sourceRangeEndUs = 0;
    if (!translateFromAnchor(request.rangeStartUs, request.boundary.ptsUs,
                             sourceBoundaryUs, &sourceRangeStartUs) ||
        !translateFromAnchor(request.rangeEndUs, request.boundary.ptsUs,
                             sourceBoundaryUs, &sourceRangeEndUs) ||
        sourceRangeEndUs <= sourceRangeStartUs) {
      return false;
    }
    // The demuxer performs a backward keyframe seek itself. Starting at the
    // requested cache edge avoids decoding an extra arbitrary lead-in GOP.
    int64_t sourceRangeStart100ns = 0;
    if (!microsecondsTo100ns(sourceRangeStartUs, &sourceRangeStart100ns) ||
        !decoder.seekToTimestamp100ns(sourceRangeStart100ns)) {
      decoder.uninit();
      decoderReady = false;
      return false;
    }

    std::vector<CachedFrame> before;
    std::vector<CachedFrame> after;
    before.reserve(32);
    after.reserve(32);
    bool boundaryFound = false;

    while (current(work.generation)) {
      VideoFrame decoded;
      VideoReadInfo info{};
      const auto decodeStart = std::chrono::steady_clock::now();
      if (!decoder.readFrame(decoded, &info, true)) {
        break;
      }
      const double decodeMs =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - decodeStart)
              .count();
      const int64_t ptsUs = decoded.timestamp100ns / 10;
      int64_t durationUs = decoded.duration100ns / 10;
      if (durationUs <= 0) {
        durationUs = request.boundary.durationUs > 0
                         ? request.boundary.durationUs
                         : kFallbackFrameDurationUs;
      }
      const FrameIdentity identity = identityFrom(info, ptsUs, durationUs);
      int64_t timelinePtsUs = 0;
      if (!translateFromAnchor(ptsUs, sourceBoundaryUs, request.boundary.ptsUs,
                               &timelinePtsUs)) {
        return false;
      }

      if (!boundaryFound &&
          sameIdentity(identity, request.boundary.identity)) {
        boundaryFound = true;
        if (request.kind == RequestKind::Around &&
            !retainBeforeFrame(decoded, info, timelinePtsUs, durationUs,
                               decodeMs, &before)) {
          return false;
        }
        if (request.kind != RequestKind::After && !before.empty()) {
          Batch batch;
          batch.serial = request.serial;
          batch.generation = work.generation;
          batch.requestKind = request.kind;
          batch.side = BatchSide::Before;
          batch.boundary = request.boundary;
          batch.frames = std::move(before);
          if (!publish(std::move(batch), work.generation)) {
            return false;
          }
        }
        if (request.kind == RequestKind::Before ||
            sourceRangeEndUs <= sourceBoundaryUs) {
          return true;
        }
        continue;
      }

      if (!boundaryFound) {
        if (ptsUs > sourceBoundaryUs) {
          return false;
        }
        if (request.kind != RequestKind::After &&
            frameEndUs(ptsUs, durationUs) > sourceRangeStartUs &&
            ptsUs <= sourceBoundaryUs) {
          if (!retainBeforeFrame(decoded, info, timelinePtsUs, durationUs,
                                 decodeMs, &before)) {
            return false;
          }
        }
        continue;
      }

      if (ptsUs >= sourceRangeEndUs) {
        break;
      }
      if (frameEndUs(ptsUs, durationUs) <= sourceBoundaryUs) {
        continue;
      }

      CachedFrame retained;
      SnapshotResult result =
          retain(decoded, info, timelinePtsUs, durationUs, decodeMs, &retained);
      if (result == SnapshotResult::BudgetFull) {
        break;
      }
      if (result == SnapshotResult::Failed) {
        return false;
      }
      after.push_back(std::move(retained));
    }

    if (!boundaryFound || !current(work.generation)) {
      return false;
    }
    if (request.kind != RequestKind::Before && !after.empty()) {
      Batch batch;
      batch.serial = request.serial;
      batch.generation = work.generation;
      batch.requestKind = request.kind;
      batch.side = BatchSide::After;
      batch.boundary = request.boundary;
      batch.frames = std::move(after);
      if (!publish(std::move(batch), work.generation)) {
        return false;
      }
    }
    return true;
  }

  void run() {
    while (true) {
      Work work;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&]() { return stopping || queued.has_value(); });
        if (stopping) {
          break;
        }
        work = *queued;
        queued.reset();
        working = true;
        workingRequest = work;
      }

      bool succeeded = false;
      try {
        succeeded = process(work);
      } catch (...) {
        // Prefetch is an optimization boundary. Allocation or decoder-wrapper
        // exceptions must fall back to the exact frame-step path, not terminate
        // the playback process.
      }

      {
        std::lock_guard<std::mutex> lock(mutex);
        if (!succeeded && work.generation == generation && !stopping) {
          failed = work;
        }
        if (working && workingRequest.generation == work.generation) {
          working = false;
        }
        cv.notify_all();
      }
    }
    decoder.uninit();
    decoderReady = false;
  }
};

Prefetcher::Prefetcher() : impl_(std::make_unique<Impl>()) {}

Prefetcher::~Prefetcher() { stop(); }

bool Prefetcher::start(const std::filesystem::path& path, int videoStreamIndex,
                       ID3D11Device* device,
                       std::recursive_mutex* contextMutex) {
  if (!impl_ || path.empty() || videoStreamIndex < 0 || !device ||
      !contextMutex) {
    return false;
  }
  stop();

  impl_->path = path;
  impl_->videoStreamIndex = videoStreamIndex;
  impl_->device = device;
  impl_->device->GetImmediateContext(&impl_->context);
  if (!impl_->context) {
    impl_->device.Reset();
    return false;
  }
  impl_->contextMutex = contextMutex;
  impl_->budget = CacheBudget::create();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stopping = false;
    impl_->started = true;
    impl_->working = false;
    impl_->queued.reset();
    impl_->failed.reset();
    impl_->batches.clear();
    ++impl_->generation;
  }
  try {
    impl_->worker = std::thread([impl = impl_.get()]() { impl->run(); });
  } catch (const std::system_error&) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->started = false;
    impl_->stopping = true;
    impl_->budget.reset();
    impl_->context.Reset();
    impl_->device.Reset();
    return false;
  }
  return true;
}

void Prefetcher::stop() {
  if (!impl_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->started && !impl_->worker.joinable()) {
      return;
    }
    impl_->stopping = true;
    impl_->started = false;
    ++impl_->generation;
    impl_->queued.reset();
    impl_->failed.reset();
    impl_->batches.clear();
    impl_->cv.notify_all();
  }
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  impl_->decoder.uninit();
  impl_->decoderReady = false;
  impl_->working = false;
  impl_->contextMutex = nullptr;
  impl_->budget.reset();
  impl_->context.Reset();
  impl_->device.Reset();
}

void Prefetcher::invalidate() {
  if (!impl_) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  ++impl_->generation;
  impl_->queued.reset();
  impl_->failed.reset();
  impl_->batches.clear();
  impl_->cv.notify_all();
}

bool Prefetcher::request(const Request& request) {
  if (!impl_ || request.serial <= 0 || !request.boundary.valid() ||
      request.rangeStartUs < 0 ||
      request.rangeEndUs <= request.rangeStartUs) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const bool completionPending =
      std::any_of(impl_->batches.begin(), impl_->batches.end(),
                  [&](const Batch& batch) {
                    return batch.generation == impl_->generation;
                  }) ||
      (impl_->failed && impl_->failed->generation == impl_->generation);
  if (!impl_->started || impl_->stopping || impl_->queued.has_value() ||
      completionPending ||
      (impl_->working &&
       impl_->workingRequest.generation == impl_->generation)) {
    return false;
  }
  ++impl_->generation;
  impl_->failed.reset();
  Impl::Work work;
  work.request = request;
  work.generation = impl_->generation;
  impl_->queued = work;
  impl_->cv.notify_all();
  return true;
}

bool Prefetcher::busyFor(
    int serial, playback_video_frame_step::Direction direction) const {
  if (!impl_ || serial <= 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto kindCovers = [&](RequestKind kind) {
    return kind == RequestKind::Around ||
           (direction == playback_video_frame_step::Direction::Previous &&
            kind == RequestKind::Before) ||
           (direction == playback_video_frame_step::Direction::Next &&
            kind == RequestKind::After);
  };
  auto covers = [&](const Impl::Work& work) {
    if (work.generation != impl_->generation ||
        work.request.serial != serial) {
      return false;
    }
    return kindCovers(work.request.kind);
  };
  auto completedBatchCovers = [&](const Batch& batch) {
    return batch.generation == impl_->generation && batch.serial == serial &&
           kindCovers(batch.requestKind);
  };

  // A completed result remains busy until the output thread adopts or observes
  // it. Otherwise a refill request can advance the generation in the narrow
  // interval between takeBatches() and this check, discarding valid work.
  return (impl_->queued && covers(*impl_->queued)) ||
         (impl_->working && covers(impl_->workingRequest)) ||
         std::any_of(impl_->batches.begin(), impl_->batches.end(),
                     completedBatchCovers) ||
         (impl_->failed && covers(*impl_->failed));
}

std::vector<Batch> Prefetcher::takeBatches(int serial) {
  std::vector<Batch> out;
  if (!impl_ || serial <= 0) {
    return out;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto it = impl_->batches.begin();
  while (it != impl_->batches.end()) {
    if (it->generation != impl_->generation) {
      it = impl_->batches.erase(it);
    } else if (it->serial == serial) {
      out.push_back(std::move(*it));
      it = impl_->batches.erase(it);
    } else {
      ++it;
    }
  }
  return out;
}

bool Prefetcher::takeFailure(int serial) {
  if (!impl_ || serial <= 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->failed ||
      impl_->failed->generation != impl_->generation ||
      impl_->failed->request.serial != serial) {
    return false;
  }
  impl_->failed.reset();
  return true;
}

}  // namespace playback_video_frame_step_prefetch
