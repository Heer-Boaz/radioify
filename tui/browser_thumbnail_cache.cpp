#include "browser_thumbnail_cache.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "asciiart.h"
#include "core/waitable_signal.h"
#include "playback/media/artwork_catalog.h"
#include "playback/target.h"
#include "playback/video/decoder.h"
#include "runtime_helpers.h"
#include "tui/ui/browser_model.h"

namespace {

enum class EntryState : std::uint8_t {
  Pending,
  Ready,
  Failed,
};

struct CacheEntry {
  EntryState state = EntryState::Pending;
  std::shared_ptr<const BrowserThumbnail> thumbnail;
};

struct ThumbnailJob {
  std::string key;
  std::filesystem::path path;
  std::optional<PlaybackTarget> audioTarget;
  BrowserThumbnailCache::MediaKind kind =
      BrowserThumbnailCache::MediaKind::Image;
  int width = 0;
  int height = 0;
  std::uint64_t generation = 0;
};

std::string cacheKey(const BrowserEntry& entry) {
  std::string key = toUtf8String(entry.path);
  if (const auto* track = entry.actionAs<browser_entry::PlayTrack>()) {
    key += "#track=" + std::to_string(track->trackIndex);
  }
  return key;
}

void assignFromAscii(const AsciiArt& art, BrowserThumbnail& out) {
  out.width = art.width;
  out.height = art.height;
  out.cells.resize(art.cells.size());
  for (std::size_t i = 0; i < art.cells.size(); ++i) {
    const auto& cell = art.cells[i];
    auto& destination = out.cells[i];
    destination.ch = cell.ch;
    destination.fg = cell.fg;
    destination.bg = cell.bg;
    destination.hasBg = cell.hasBg;
  }
}

bool renderImage(const std::filesystem::path& file, int maxWidth,
                 int maxHeight, BrowserThumbnail& out, std::string* error) {
  AsciiArt art;
  if (!renderAsciiArt(file, maxWidth, maxHeight, art, error)) return false;
  assignFromAscii(art, out);
  return true;
}

void computeVideoTarget(int thumbnailWidth, int thumbnailHeight, int& outWidth,
                        int& outHeight) {
  outWidth = (std::max)(2, thumbnailWidth * 2);
  outHeight = (std::max)(4, thumbnailHeight * 4);
  if (outWidth & 1) ++outWidth;
  if (outHeight & 1) ++outHeight;
}

bool renderVideo(const std::filesystem::path& file, int maxWidth,
                 int maxHeight, BrowserThumbnail& out, std::string* error) {
  if (maxWidth <= 0 || maxHeight <= 0) return false;
  VideoDecoder decoder;
  std::string initError;
  if (!decoder.init(file, &initError, true, false) &&
      !decoder.init(file, &initError, false, false)) {
    if (error && !initError.empty()) *error = initError;
    return false;
  }

  int targetWidth = 0;
  int targetHeight = 0;
  computeVideoTarget(maxWidth, maxHeight, targetWidth, targetHeight);
  decoder.setTargetSize(targetWidth, targetHeight, nullptr);

  VideoFrame frame;
  VideoReadInfo info;
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!decoder.readFrame(frame, &info, true)) {
      if (decoder.atEnd()) break;
      continue;
    }

    AsciiArt art;
    bool rendered = false;
    if ((frame.format == VideoPixelFormat::NV12 ||
         frame.format == VideoPixelFormat::P010) &&
        !frame.yuv.empty()) {
      const YuvFormat format = frame.format == VideoPixelFormat::P010
                                   ? YuvFormat::P010
                                   : YuvFormat::NV12;
      rendered = renderAsciiArtFromYuv(
          frame.yuv.data(), frame.width, frame.height, frame.stride,
          frame.planeHeight, format, frame.fullRange, frame.yuvMatrix,
          frame.yuvTransfer, maxWidth, maxHeight, art);
    } else if (!frame.rgba.empty()) {
      rendered = renderAsciiArtFromRgba(
          frame.rgba.data(), frame.width, frame.height, maxWidth, maxHeight,
          art, true);
    }
    if (rendered) {
      assignFromAscii(art, out);
      return true;
    }
  }
  if (error && !initError.empty()) *error = initError;
  return false;
}

bool renderAudio(const PlaybackTarget& target, int maxWidth, int maxHeight,
                 BrowserThumbnail& out, std::string* error) {
  PlaybackMediaDisplayRequest request(target, false);
  AsciiArt art;
  if (!resolvePlaybackMediaArtworkAscii(
          request, MediaArtworkSidecarPolicy::FileSpecificOnly, maxWidth,
          maxHeight, &art, error)) {
    return false;
  }
  assignFromAscii(art, out);
  return true;
}

bool executeJob(const ThumbnailJob& job, BrowserThumbnail& thumbnail,
                std::string* error) {
  try {
    switch (job.kind) {
      case BrowserThumbnailCache::MediaKind::Image:
        return renderImage(job.path, job.width, job.height, thumbnail, error);
      case BrowserThumbnailCache::MediaKind::Video:
        return renderVideo(job.path, job.width, job.height, thumbnail, error);
      case BrowserThumbnailCache::MediaKind::Audio:
        return job.audioTarget &&
               renderAudio(*job.audioTarget, job.width, job.height, thumbnail,
                           error);
    }
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
  } catch (...) {
    if (error) *error = "Thumbnail preview threw an unknown exception.";
  }
  return false;
}

#ifdef _WIN32
bool executeJobGuarded(const ThumbnailJob& job, BrowserThumbnail& thumbnail,
                       std::string* error) {
  // Preview decode must never be able to take down the whole TUI.
  __try {
    return executeJob(job, thumbnail, error);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (error) *error = "Thumbnail preview decoder faulted.";
    return false;
  }
}
#else
bool executeJobGuarded(const ThumbnailJob& job, BrowserThumbnail& thumbnail,
                       std::string* error) {
  return executeJob(job, thumbnail, error);
}
#endif

}  // namespace

struct BrowserThumbnailCache::Impl {
  static constexpr std::size_t kMaximumQueuedJobs = 64;

  Impl() : worker([this]() { workerLoop(); }) {}

  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
      ++generation;
      queue.clear();
    }
    workAvailable.notify_all();
    if (worker.joinable()) worker.join();
  }

  void workerLoop() {
    for (;;) {
      ThumbnailJob job;
      {
        std::unique_lock<std::mutex> lock(mutex);
        workAvailable.wait(lock,
                           [this]() { return stopping || !queue.empty(); });
        if (stopping) return;
        job = std::move(queue.front());
        queue.pop_front();
      }

      BrowserThumbnail thumbnail;
      std::string error;
      const bool succeeded = executeJobGuarded(job, thumbnail, &error);

      {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) return;
        if (job.generation != generation) continue;
        const auto entry = entries.find(job.key);
        if (entry == entries.end()) continue;
        if (succeeded) {
          entry->second.state = EntryState::Ready;
          entry->second.thumbnail =
              std::make_shared<BrowserThumbnail>(std::move(thumbnail));
        } else {
          entry->second.state = EntryState::Failed;
          entry->second.thumbnail.reset();
        }
      }
      ready.signal();
    }
  }

  int targetWidth = 0;
  int targetHeight = 0;
  std::uint64_t generation = 1;
  std::unordered_map<std::string, CacheEntry> entries;
  std::deque<ThumbnailJob> queue;
  std::mutex mutex;
  std::condition_variable workAvailable;
  bool stopping = false;
  WaitableSignal ready;
  std::thread worker;
};

BrowserThumbnailCache::BrowserThumbnailCache()
    : impl_(std::make_unique<Impl>()) {}

BrowserThumbnailCache::~BrowserThumbnailCache() = default;

void BrowserThumbnailCache::configureTargetSize(int width, int height) {
  if (width <= 0 || height <= 0) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->targetWidth == width && impl_->targetHeight == height) return;
  impl_->targetWidth = width;
  impl_->targetHeight = height;
  ++impl_->generation;
  impl_->entries.clear();
  impl_->queue.clear();
  impl_->ready.clear();
}

BrowserThumbnailCache::Lookup BrowserThumbnailCache::lookupOrRequest(
    const BrowserEntry& entry, MediaKind kind, int width, int height,
    bool allowEnqueue) {
  Lookup result;
  if (width <= 0 || height <= 0) return result;

  const std::string key = cacheKey(entry);
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->entries.find(key);
    if (found != impl_->entries.end()) {
      if (found->second.state == EntryState::Ready) {
        result.thumbnail = found->second.thumbnail;
      } else if (found->second.state == EntryState::Pending) {
        result.pending = true;
      }
      return result;
    }
    if (!allowEnqueue || impl_->stopping ||
        impl_->queue.size() >= Impl::kMaximumQueuedJobs) {
      return result;
    }

    impl_->entries.emplace(key, CacheEntry{});
    ThumbnailJob job;
    job.key = key;
    job.path = entry.path;
    job.kind = kind;
    if (kind == MediaKind::Audio) {
      if (const auto* track = entry.actionAs<browser_entry::PlayTrack>()) {
        job.audioTarget = playbackTrackTarget(entry.path, track->trackIndex);
      } else {
        job.audioTarget = playbackFileTarget(entry.path);
      }
    }
    job.width = width;
    job.height = height;
    job.generation = impl_->generation;
    impl_->queue.push_back(std::move(job));
    result.pending = true;
    result.enqueued = true;
  }
  impl_->workAvailable.notify_one();
  return result;
}

NativeWaitHandle BrowserThumbnailCache::waitHandle() const {
  return impl_->ready.nativeWaitHandle();
}

bool BrowserThumbnailCache::consumeReady() { return impl_->ready.consume(); }
