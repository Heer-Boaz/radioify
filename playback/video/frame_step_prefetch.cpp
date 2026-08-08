#include "playback/video/frame_step_prefetch.h"

#include "playback/video/frame_step_source_cache.h"

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

bool requestCoversDirection(const Request& request,
                            playback_video_frame_step::Direction direction) {
  return request.direction == direction;
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

class SnapshotCommandBatch {
 public:
  SnapshotCommandBatch(ID3D11DeviceContext* deferredContext,
                       ID3D11DeviceContext* immediateContext,
                       std::recursive_mutex* immediateContextMutex)
      : deferredContext_(deferredContext),
        immediateContext_(immediateContext),
        immediateContextMutex_(immediateContextMutex) {}

  ~SnapshotCommandBatch() { discard(); }

  bool copy(ID3D11Texture2D* destination, ID3D11Texture2D* source,
            UINT sourceSubresource) {
    if (!destination || !source || !immediateContext_ ||
        !immediateContextMutex_) {
      return false;
    }
    if (!deferredContext_) {
      std::lock_guard<std::recursive_mutex> lock(*immediateContextMutex_);
      immediateContext_->CopySubresourceRegion(
          destination, 0, 0, 0, 0, source, sourceSubresource, nullptr);
      return true;
    }

    deferredContext_->CopySubresourceRegion(
        destination, 0, 0, 0, 0, source, sourceSubresource, nullptr);
    sourceTextures_.emplace_back(source);
    hasCommands_ = true;
    return true;
  }

  bool submit() {
    if (!hasCommands_) {
      return true;
    }
    Microsoft::WRL::ComPtr<ID3D11CommandList> commandList;
    if (!deferredContext_ ||
        FAILED(deferredContext_->FinishCommandList(FALSE, &commandList)) ||
        !commandList) {
      reset();
      return false;
    }
    {
      std::lock_guard<std::recursive_mutex> lock(*immediateContextMutex_);
      immediateContext_->ExecuteCommandList(commandList.Get(), FALSE);
    }
    hasCommands_ = false;
    sourceTextures_.clear();
    return true;
  }

 private:
  void discard() {
    if (hasCommands_ && deferredContext_) {
      Microsoft::WRL::ComPtr<ID3D11CommandList> discarded;
      deferredContext_->FinishCommandList(FALSE, &discarded);
    }
    reset();
  }

  void reset() {
    hasCommands_ = false;
    sourceTextures_.clear();
  }

  ID3D11DeviceContext* deferredContext_ = nullptr;
  ID3D11DeviceContext* immediateContext_ = nullptr;
  std::recursive_mutex* immediateContextMutex_ = nullptr;
  bool hasCommands_ = false;
  std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> sourceTextures_;
};

SnapshotResult snapshotFrame(
    const VideoFrame& decoded, const std::shared_ptr<CacheBudget>& budget,
    ID3D11Device* device, SnapshotCommandBatch* commands, VideoFrame* out) {
  if (!out || !budget || !commands) {
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

  if (!decoded.hwTexture || !device) {
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
  // ID3D11Device resource creation is free-threaded on the shared device. Keep
  // the explicit cross-pipeline lock scoped to the immediate-context command;
  // otherwise allocation latency unnecessarily blocks presentation.
  if (FAILED(device->CreateTexture2D(&snapshotDesc, nullptr, &snapshot))) {
    return SnapshotResult::Failed;
  }
  // FFmpeg represents a D3D11VA frame as one array subresource. Record all
  // snapshots on the worker-owned deferred context and submit them as one
  // command list so presentation only takes the shared immediate-context lock
  // once per completed cache transaction.
  if (!commands->copy(snapshot.Get(), decoded.hwTexture.Get(),
                      static_cast<UINT>(decoded.hwTextureArrayIndex))) {
    return SnapshotResult::Failed;
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
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> deferredContext;
  std::recursive_mutex* contextMutex = nullptr;
  std::shared_ptr<CacheBudget> budget;
  VideoDecoder decoder;
  bool decoderReady = false;
  std::optional<FrameIdentity> decoderTailIdentity;

  mutable std::mutex mutex;
  std::condition_variable cv;
  std::optional<Work> queued;
  std::optional<Work> failed;
  std::vector<Result> results;
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
    decoderTailIdentity.reset();
    return decoderReady;
  }

  bool publish(Result result, uint64_t candidateGeneration) {
    std::lock_guard<std::mutex> lock(mutex);
    if (stopping || candidateGeneration != generation) {
      return false;
    }
    results.push_back(std::move(result));
    cv.notify_all();
    return true;
  }

  SnapshotResult retain(const VideoFrame& decoded, const VideoReadInfo& info,
                        int64_t sourcePtsUs, int64_t durationUs,
                        double decodeMs, SnapshotCommandBatch* commands,
                        std::shared_ptr<const SourceFrame>* out) {
    if (!out) {
      return SnapshotResult::Failed;
    }
    auto retained = std::make_shared<SourceFrame>();
    SnapshotResult result = snapshotFrame(decoded, budget, device.Get(),
                                          commands, &retained->frame);
    if (result != SnapshotResult::Ok) {
      return result;
    }
    retained->info = info;
    retained->sourcePtsUs = sourcePtsUs;
    retained->durationUs = durationUs;
    retained->decodeMs = decodeMs;
    retained->identity = identityFrom(info, sourcePtsUs, durationUs);
    *out = std::move(retained);
    return SnapshotResult::Ok;
  }

  bool process(const Work& work) {
    if (!ensureDecoder() || !current(work.generation)) {
      return false;
    }

    const Request& request = work.request;
    constexpr int64_t kUnknown = (std::numeric_limits<int64_t>::min)();
    if (!request.valid() || request.boundary.sourcePtsUs == kUnknown ||
        request.join.sourcePtsUs == kUnknown) {
      return false;
    }

    const bool previous =
        request.direction == playback_video_frame_step::Direction::Previous;
    bool joined = !previous && request.joinCached && decoderTailIdentity &&
                  sameIdentity(*decoderTailIdentity, request.join.identity);
    const bool decoderSeeked = !joined;
    if (decoderSeeked) {
      const int64_t seekUs =
          previous ? request.rangeStartUs : request.join.sourcePtsUs;
      int64_t seek100ns = 0;
      if (!microsecondsTo100ns(seekUs, &seek100ns) ||
          !decoder.seekToTimestamp100ns(seek100ns)) {
        decoder.uninit();
        decoderReady = false;
        decoderTailIdentity.reset();
        return false;
      }
      decoderTailIdentity.reset();
    }

    SnapshotCommandBatch snapshotCommands(deferredContext.Get(), context.Get(),
                                          contextMutex);
    std::vector<std::shared_ptr<const SourceFrame>> stagedFrames;
    std::shared_ptr<const SourceFrame> stagedJoinFrame;
    size_t decodedFrameCount = 0;
    bool reachedMediaBoundary = false;
    bool decodeComplete = false;
    while (!decodeComplete && current(work.generation)) {
      VideoFrame decoded;
      VideoReadInfo info{};
      const auto decodeStart = std::chrono::steady_clock::now();
      if (!decoder.readFrame(decoded, &info, true)) {
        break;
      }
      ++decodedFrameCount;
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
      decoderTailIdentity = identity;

      if (!joined) {
        if (sameIdentity(identity, request.join.identity)) {
          joined = true;
          if (!request.joinCached) {
            const SnapshotResult retainResult =
                retain(decoded, info, ptsUs, durationUs, decodeMs,
                       &snapshotCommands, &stagedJoinFrame);
            if (retainResult != SnapshotResult::Ok) {
              return false;
            }
          }
          if (previous) {
            decodeComplete = true;
          }
          continue;
        }

        // A seek must reproduce the exact cached edge before its timestamp is
        // crossed. Accepting a merely nearby PTS would manufacture adjacency
        // and is the source of frame skips around keyframe boundaries.
        if (ptsUs > request.join.sourcePtsUs) {
          return false;
        }

        if (!previous ||
            frameEndUs(ptsUs, durationUs) <= request.rangeStartUs) {
          continue;
        }
      }

      if (joined && previous) {
        decodeComplete = true;
        continue;
      }

      if (joined || previous) {
        const bool alreadyStaged = std::any_of(
            stagedFrames.begin(), stagedFrames.end(),
            [&](const std::shared_ptr<const SourceFrame>& candidate) {
              return sameIdentity(candidate->identity, identity);
            });
        if (alreadyStaged) {
          return false;
        }
        std::shared_ptr<const SourceFrame> retained;
        const SnapshotResult retainResult =
            retain(decoded, info, ptsUs, durationUs, decodeMs,
                   &snapshotCommands, &retained);
        if (retainResult != SnapshotResult::Ok) {
          return false;
        }
        stagedFrames.push_back(std::move(retained));
      }

      if (!previous && joined &&
          frameEndUs(ptsUs, durationUs) >= request.rangeEndUs) {
        decodeComplete = true;
      }
    }

    if (!decodeComplete && joined && decoder.atEnd()) {
      reachedMediaBoundary = true;
      decodeComplete = true;
    }
    if (previous && joined && request.rangeStartUs == 0) {
      reachedMediaBoundary = true;
    }

    if (!decodeComplete || !joined || !current(work.generation)) {
      return false;
    }
    if (!snapshotCommands.submit()) {
      return false;
    }

    Result result;
    result.request = request;
    result.generation = work.generation;
    result.decoderSeeked = decoderSeeked;
    result.reachedMediaBoundary = reachedMediaBoundary;
    result.decodedFrameCount = decodedFrameCount;
    result.joinFrame = std::move(stagedJoinFrame);
    result.frames = std::move(stagedFrames);
    return publish(std::move(result), work.generation);
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
        } else if (succeeded && work.generation == generation) {
          failed.reset();
        }
        if (working && workingRequest.generation == work.generation) {
          working = false;
        }
        cv.notify_all();
      }
    }
    decoder.uninit();
    decoderReady = false;
    decoderTailIdentity.reset();
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
  // Deferred contexts are the D3D11 mechanism for recording GPU work on a
  // worker thread. Failure is non-fatal: the snapshot batch retains a correct
  // immediate-context fallback for devices that do not expose one.
  if (FAILED(
          impl_->device->CreateDeferredContext(0, &impl_->deferredContext))) {
    impl_->deferredContext.Reset();
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
    impl_->results.clear();
    impl_->decoderTailIdentity.reset();
    ++impl_->generation;
  }
  try {
    impl_->worker = std::thread([impl = impl_.get()]() { impl->run(); });
  } catch (const std::system_error&) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->started = false;
    impl_->stopping = true;
    impl_->budget.reset();
    impl_->deferredContext.Reset();
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
    impl_->results.clear();
    impl_->cv.notify_all();
  }
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  impl_->decoder.uninit();
  impl_->decoderReady = false;
  impl_->decoderTailIdentity.reset();
  impl_->working = false;
  impl_->contextMutex = nullptr;
  impl_->budget.reset();
  impl_->deferredContext.Reset();
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
  impl_->results.clear();
  impl_->cv.notify_all();
}

bool Prefetcher::request(const Request& request) {
  if (!impl_ || !request.valid()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const bool completionPending =
      std::any_of(impl_->results.begin(), impl_->results.end(),
                  [&](const Result& result) {
                    return result.generation == impl_->generation;
                  }) ||
      (impl_->failed && impl_->failed->generation == impl_->generation);
  if (!impl_->started || impl_->stopping || completionPending) {
    return false;
  }

  const auto sameDirectionWork = [&](const Impl::Work& work) {
    return work.generation == impl_->generation &&
           work.request.serial == request.serial &&
           work.request.direction == request.direction;
  };
  if ((impl_->queued && sameDirectionWork(*impl_->queued)) ||
      (impl_->working && sameDirectionWork(impl_->workingRequest))) {
    return false;
  }

  // A direction change is a priority change. Supersede queued/in-flight work;
  // the worker observes the generation mismatch and discards its unsubmitted
  // decoder/GPU transaction before taking this request.
  ++impl_->generation;
  impl_->failed.reset();
  impl_->results.clear();
  Impl::Work work;
  work.request = request;
  work.generation = impl_->generation;
  impl_->queued = work;
  impl_->cv.notify_all();
  return true;
}

bool Prefetcher::busyFor(int serial,
                         playback_video_frame_step::Direction direction) const {
  if (!impl_ || serial <= 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto covers = [&](const Impl::Work& work) {
    if (work.generation != impl_->generation || work.request.serial != serial) {
      return false;
    }
    return requestCoversDirection(work.request, direction);
  };
  auto completedResultCovers = [&](const Result& result) {
    return result.generation == impl_->generation &&
           result.request.serial == serial &&
           requestCoversDirection(result.request, direction);
  };

  // A completed result remains busy until the output thread adopts or observes
  // it. Otherwise a refill request can advance the generation in the narrow
  // interval between takeResults() and this check, discarding valid work.
  return (impl_->queued && covers(*impl_->queued)) ||
         (impl_->working && covers(impl_->workingRequest)) ||
         std::any_of(impl_->results.begin(), impl_->results.end(),
                     completedResultCovers) ||
         (impl_->failed && covers(*impl_->failed));
}

bool Prefetcher::waitUntilIdleOrCompleted(
    int serial, playback_video_frame_step::Direction direction,
    std::chrono::milliseconds timeout) const {
  if (!impl_ || serial <= 0 || timeout < std::chrono::milliseconds::zero()) {
    return false;
  }

  std::unique_lock<std::mutex> lock(impl_->mutex);
  const auto idleOrCompleted = [&]() {
    const auto covers = [&](const Impl::Work& work) {
      return work.generation == impl_->generation &&
             work.request.serial == serial &&
             requestCoversDirection(work.request, direction);
    };
    const bool workPending = (impl_->queued && covers(*impl_->queued)) ||
                             (impl_->working && covers(impl_->workingRequest));
    const bool resultReady =
        std::any_of(impl_->results.begin(), impl_->results.end(),
                    [&](const Result& result) {
                      return result.generation == impl_->generation &&
                             result.request.serial == serial &&
                             requestCoversDirection(result.request, direction);
                    });
    const bool failureReady = impl_->failed && covers(*impl_->failed);
    return impl_->stopping || !impl_->started || resultReady || failureReady ||
           !workPending;
  };
  return impl_->cv.wait_for(lock, timeout, idleOrCompleted);
}

std::vector<Result> Prefetcher::takeResults(int serial) {
  std::vector<Result> out;
  if (!impl_ || serial <= 0) {
    return out;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto it = impl_->results.begin();
  while (it != impl_->results.end()) {
    if (it->generation != impl_->generation) {
      it = impl_->results.erase(it);
    } else if (it->request.serial == serial) {
      out.push_back(std::move(*it));
      it = impl_->results.erase(it);
    } else {
      ++it;
    }
  }
  return out;
}

std::optional<Request> Prefetcher::takeFailure(int serial) {
  if (!impl_ || serial <= 0) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->failed || impl_->failed->generation != impl_->generation ||
      impl_->failed->request.serial != serial) {
    return std::nullopt;
  }
  Request failedRequest = impl_->failed->request;
  impl_->failed.reset();
  return failedRequest;
}

}  // namespace playback_video_frame_step_prefetch
