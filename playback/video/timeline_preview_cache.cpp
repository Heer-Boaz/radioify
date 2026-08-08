#include "playback/video/timeline_preview_cache.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <list>
#include <limits>
#include <map>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include "core/runtime_helpers.h"

namespace playback_video_timeline_preview {
namespace {

constexpr std::array<uint8_t, 8> kCacheMagic = {'R', 'F', 'T', 'H', 'U', 'M',
                                                 'B', '1'};
constexpr uint32_t kCacheVersion = 1;
constexpr const wchar_t* kCacheExtension = L".rfthumb";

template <typename T>
bool writeValue(std::ofstream& stream, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
  return stream.good();
}

template <typename T>
bool readValue(std::ifstream& stream, T* value) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (!value) return false;
  stream.read(reinterpret_cast<char*>(value), sizeof(*value));
  return stream.good();
}

std::string sha256Hex(const std::string& value) {
  AVSHA* context = av_sha_alloc();
  if (!context) return {};
  std::array<uint8_t, 32> digest{};
  const int initResult = av_sha_init(context, 256);
  if (initResult == 0) {
    av_sha_update(context, reinterpret_cast<const uint8_t*>(value.data()),
                  value.size());
    av_sha_final(context, digest.data());
  }
  av_free(context);
  if (initResult != 0) return {};

  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const uint8_t byte : digest) {
    out << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return out.str();
}

std::string sourceIdentity(const Source& source) {
  std::error_code ec;
  std::filesystem::path normalized =
      std::filesystem::weakly_canonical(source.path, ec);
  if (ec || normalized.empty()) {
    ec.clear();
    normalized = std::filesystem::absolute(source.path, ec);
  }
  if (ec || normalized.empty()) normalized = source.path;

  ec.clear();
  const uintmax_t fileSize = std::filesystem::file_size(source.path, ec);
  const uintmax_t stableSize = ec ? 0 : fileSize;
  ec.clear();
  const auto modified = std::filesystem::last_write_time(source.path, ec);
  const auto modifiedTicks =
      ec ? int64_t{0} : static_cast<int64_t>(modified.time_since_epoch().count());

  std::ostringstream identity;
  identity << "radioify-timeline-preview-v1\n"
           << toUtf8String(normalized) << '\n' << stableSize << '\n'
           << modifiedTicks << '\n' << source.videoStreamIndex << '\n'
           << source.durationUs << '\n' << source.sourceWidth << 'x'
           << source.sourceHeight << '\n' << kDecodeMaxWidth << 'x'
           << kDecodeMaxHeight;
  return identity.str();
}

size_t framePayloadBytes(const VideoFrame& frame) {
  if (frame.rgba.size() > (std::numeric_limits<size_t>::max)() -
                              frame.yuv.size()) {
    return 0;
  }
  return frame.rgba.size() + frame.yuv.size();
}

bool supportedPersistentFrame(const VideoFrame& frame) {
  if (frame.width <= 0 || frame.height <= 0 ||
      frame.width > kDecodeMaxWidth || frame.height > kDecodeMaxHeight) {
    return false;
  }
  const size_t payloadBytes = framePayloadBytes(frame);
  if (payloadBytes == 0 || payloadBytes > kMaxCacheBytes) return false;
  switch (frame.format) {
    case VideoPixelFormat::NV12:
    case VideoPixelFormat::P010: {
      if (!frame.rgba.empty() || frame.yuv.empty() || frame.stride <= 0 ||
          frame.planeHeight < frame.height || (frame.width & 1) != 0 ||
          (frame.height & 1) != 0) {
        return false;
      }
      const uint64_t bytesPerSample =
          frame.format == VideoPixelFormat::P010 ? 2u : 1u;
      if (static_cast<uint64_t>(frame.stride) <
          static_cast<uint64_t>(frame.width) * bytesPerSample) {
        return false;
      }
      const uint64_t yBytes = static_cast<uint64_t>(frame.stride) *
                              static_cast<uint64_t>(frame.planeHeight);
      const uint64_t expectedBytes =
          yBytes + static_cast<uint64_t>(frame.stride) *
                       static_cast<uint64_t>(frame.height / 2);
      return expectedBytes == frame.yuv.size();
    }
    case VideoPixelFormat::RGB32:
    case VideoPixelFormat::ARGB32: {
      if (!frame.yuv.empty() || frame.rgba.empty()) return false;
      const uint64_t minimumStride =
          static_cast<uint64_t>(frame.width) * 4u;
      const uint64_t stride =
          frame.stride > 0 ? static_cast<uint64_t>(frame.stride)
                           : minimumStride;
      if (stride < minimumStride) return false;
      const uint64_t expected =
          stride * static_cast<uint64_t>(frame.height);
      return expected == frame.rgba.size();
    }
    default:
      return false;
  }
}

bool validFormat(uint32_t value) {
  return value == static_cast<uint32_t>(VideoPixelFormat::NV12) ||
         value == static_cast<uint32_t>(VideoPixelFormat::P010) ||
         value == static_cast<uint32_t>(VideoPixelFormat::RGB32) ||
         value == static_cast<uint32_t>(VideoPixelFormat::ARGB32);
}

bool saveImage(const std::filesystem::path& path, const Image& image) {
  if (!supportedPersistentFrame(image.frame)) return false;
  static std::atomic<uint64_t> temporaryId{1};
  std::filesystem::path temporary = path;
  temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(
                   temporaryId.fetch_add(1, std::memory_order_relaxed));

  std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
  if (!stream) return false;
  stream.write(reinterpret_cast<const char*>(kCacheMagic.data()),
               kCacheMagic.size());
  const VideoFrame& frame = image.frame;
  const uint32_t format = static_cast<uint32_t>(frame.format);
  const uint32_t matrix = static_cast<uint32_t>(frame.yuvMatrix);
  const uint32_t transfer = static_cast<uint32_t>(frame.yuvTransfer);
  const uint8_t fullRange = frame.fullRange ? 1 : 0;
  const uint64_t rgbaSize = frame.rgba.size();
  const uint64_t yuvSize = frame.yuv.size();
  bool ok = stream.good() && writeValue(stream, kCacheVersion) &&
            writeValue(stream, image.requestedUs) &&
            writeValue(stream, image.frameUs) && writeValue(stream, frame.width) &&
            writeValue(stream, frame.height) &&
            writeValue(stream, frame.timestamp100ns) &&
            writeValue(stream, frame.duration100ns) &&
            writeValue(stream, format) &&
            writeValue(stream, frame.rotationQuarterTurns) &&
            writeValue(stream, frame.stride) &&
            writeValue(stream, frame.planeHeight) &&
            writeValue(stream, fullRange) && writeValue(stream, matrix) &&
            writeValue(stream, transfer) && writeValue(stream, rgbaSize) &&
            writeValue(stream, yuvSize);
  if (ok && rgbaSize > 0) {
    stream.write(reinterpret_cast<const char*>(frame.rgba.data()),
                 static_cast<std::streamsize>(rgbaSize));
    ok = stream.good();
  }
  if (ok && yuvSize > 0) {
    stream.write(reinterpret_cast<const char*>(frame.yuv.data()),
                 static_cast<std::streamsize>(yuvSize));
    ok = stream.good();
  }
  stream.flush();
  ok = ok && stream.good();
  stream.close();
  if (ok) {
    ok = MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
         FALSE;
  }
  if (!ok) {
    std::error_code ec;
    std::filesystem::remove(temporary, ec);
  }
  return ok;
}

std::shared_ptr<const Image> loadImage(const std::filesystem::path& path,
                                       int64_t expectedTargetUs,
                                       uint64_t imageId) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::array<uint8_t, kCacheMagic.size()> magic{};
  stream.read(reinterpret_cast<char*>(magic.data()), magic.size());
  if (!stream.good() || magic != kCacheMagic) return {};

  uint32_t version = 0;
  auto image = std::make_shared<Image>();
  image->id = imageId;
  VideoFrame& frame = image->frame;
  uint32_t format = 0;
  uint32_t matrix = 0;
  uint32_t transfer = 0;
  uint8_t fullRange = 0;
  uint64_t rgbaSize = 0;
  uint64_t yuvSize = 0;
  const bool headerOk =
      readValue(stream, &version) && readValue(stream, &image->requestedUs) &&
      readValue(stream, &image->frameUs) && readValue(stream, &frame.width) &&
      readValue(stream, &frame.height) &&
      readValue(stream, &frame.timestamp100ns) &&
      readValue(stream, &frame.duration100ns) && readValue(stream, &format) &&
      readValue(stream, &frame.rotationQuarterTurns) &&
      readValue(stream, &frame.stride) &&
      readValue(stream, &frame.planeHeight) && readValue(stream, &fullRange) &&
      readValue(stream, &matrix) && readValue(stream, &transfer) &&
      readValue(stream, &rgbaSize) && readValue(stream, &yuvSize);
  if (!headerOk || version != kCacheVersion ||
      image->requestedUs != expectedTargetUs || !validFormat(format) ||
      matrix > static_cast<uint32_t>(YuvMatrix::Bt2020) ||
      transfer > static_cast<uint32_t>(YuvTransfer::Hlg) || fullRange > 1 ||
      rgbaSize > kMaxCacheBytes || yuvSize > kMaxCacheBytes ||
      rgbaSize > kMaxCacheBytes - std::min<uint64_t>(kMaxCacheBytes, yuvSize)) {
    return {};
  }
  frame.format = static_cast<VideoPixelFormat>(format);
  frame.yuvMatrix = static_cast<YuvMatrix>(matrix);
  frame.yuvTransfer = static_cast<YuvTransfer>(transfer);
  frame.fullRange = fullRange != 0;
  frame.rgba.resize(static_cast<size_t>(rgbaSize));
  frame.yuv.resize(static_cast<size_t>(yuvSize));
  if (rgbaSize > 0) {
    stream.read(reinterpret_cast<char*>(frame.rgba.data()),
                static_cast<std::streamsize>(rgbaSize));
  }
  if (stream.good() && yuvSize > 0) {
    stream.read(reinterpret_cast<char*>(frame.yuv.data()),
                static_cast<std::streamsize>(yuvSize));
  }
  if (!stream.good() || stream.peek() != std::char_traits<char>::eof()) {
    return {};
  }
  frame.storageBytes = framePayloadBytes(frame);
  if (!supportedPersistentFrame(frame)) return {};
  return image;
}

}  // namespace

struct Cache::Impl {
  struct Entry {
    std::shared_ptr<const Image> image;
    size_t bytes = 0;
    std::list<int64_t>::iterator lru;
  };

  explicit Impl(CacheConfig value) : config(std::move(value)) {}

  CacheConfig config;
  std::filesystem::path root;
  std::filesystem::path directory;
  std::map<int64_t, Entry> entries;
  std::list<int64_t> lru;
  size_t bytes = 0;

  std::filesystem::path fileFor(int64_t targetUs) const {
    return directory / (std::to_wstring(targetUs) + kCacheExtension);
  }

  void insertMemory(const std::shared_ptr<const Image>& image) {
    if (!image) return;
    const size_t cost = framePayloadBytes(image->frame);
    if (cost == 0 || cost > config.memoryByteLimit ||
        config.memoryFrameLimit == 0) {
      return;
    }
    auto existing = entries.find(image->requestedUs);
    if (existing != entries.end()) {
      bytes -= existing->second.bytes;
      lru.erase(existing->second.lru);
      entries.erase(existing);
    }
    lru.push_front(image->requestedUs);
    entries.emplace(image->requestedUs,
                    Entry{image, cost, lru.begin()});
    bytes += cost;
    while (!lru.empty() &&
           (bytes > config.memoryByteLimit ||
            entries.size() > config.memoryFrameLimit)) {
      const int64_t oldest = lru.back();
      const auto it = entries.find(oldest);
      if (it != entries.end()) {
        bytes -= it->second.bytes;
        entries.erase(it);
      }
      lru.pop_back();
    }
  }
};

Cache::Cache(CacheConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Cache::~Cache() = default;

bool Cache::open(const Source& source, std::string* error) {
  close();
  if (source.path.empty() || source.videoStreamIndex < 0 ||
      source.durationUs <= 0 || source.sourceWidth <= 0 ||
      source.sourceHeight <= 0) {
    if (error) *error = "Invalid timeline thumbnail cache source.";
    return false;
  }
  impl_->root = impl_->config.persistentRoot.empty()
                    ? radioifyWritableDataDir() / "cache" /
                          "timeline-preview-v1"
                    : impl_->config.persistentRoot;
  if (impl_->config.persistentByteLimit == 0 || impl_->root.empty()) {
    impl_->root.clear();
    return true;
  }
  const std::string identityHash = sha256Hex(sourceIdentity(source));
  if (identityHash.empty()) {
    impl_->root.clear();
    return true;
  }
  impl_->directory = impl_->root / identityHash;
  std::error_code ec;
  std::filesystem::create_directories(impl_->directory, ec);
  if (ec || !std::filesystem::is_directory(impl_->directory, ec) || ec) {
    impl_->root.clear();
    impl_->directory.clear();
  }
  return true;
}

void Cache::close() {
  impl_->entries.clear();
  impl_->lru.clear();
  impl_->bytes = 0;
  impl_->root.clear();
  impl_->directory.clear();
}

CacheLookup Cache::find(int64_t targetUs, uint64_t imageId) {
  CacheLookup lookup;
  const auto cached = impl_->entries.find(targetUs);
  if (cached != impl_->entries.end()) {
    impl_->lru.erase(cached->second.lru);
    impl_->lru.push_front(targetUs);
    cached->second.lru = impl_->lru.begin();
    lookup.image = cached->second.image;
    lookup.origin = ResultOrigin::MemoryCache;
    return lookup;
  }
  if (impl_->directory.empty()) return lookup;

  const std::filesystem::path path = impl_->fileFor(targetUs);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) return lookup;
  lookup.image = loadImage(path, targetUs, imageId);
  if (!lookup.image) {
    ec.clear();
    std::filesystem::remove(path, ec);
    return lookup;
  }
  impl_->insertMemory(lookup.image);
  lookup.origin = ResultOrigin::PersistentCache;
  ec.clear();
  std::filesystem::last_write_time(
      path, std::filesystem::file_time_type::clock::now(), ec);
  return lookup;
}

void Cache::storeMemory(const std::shared_ptr<const Image>& image) {
  if (!image || !supportedPersistentFrame(image->frame)) return;
  impl_->insertMemory(image);
}

void Cache::storePersistent(const std::shared_ptr<const Image>& image) {
  if (!image || !supportedPersistentFrame(image->frame)) return;
  if (impl_->directory.empty()) return;
  saveImage(impl_->fileFor(image->requestedUs), *image);
}

void Cache::prunePersistent() {
  if (impl_->root.empty() || impl_->config.persistentByteLimit == 0) return;
  struct File {
    std::filesystem::path path;
    uint64_t bytes = 0;
    std::filesystem::file_time_type modified;
  };
  std::vector<File> files;
  uint64_t totalBytes = 0;
  std::error_code ec;
  std::filesystem::recursive_directory_iterator iterator(
      impl_->root, std::filesystem::directory_options::skip_permission_denied,
      ec);
  const std::filesystem::recursive_directory_iterator end;
  while (!ec && iterator != end) {
    const auto& entry = *iterator;
    if (entry.is_regular_file(ec) && !ec) {
      const std::filesystem::path entryPath = entry.path();
      const auto modified = entry.last_write_time(ec);
      if (!ec &&
          entryPath.filename().wstring().find(L".rfthumb.tmp-") !=
              std::wstring::npos) {
        if (std::filesystem::file_time_type::clock::now() - modified >
            std::chrono::hours(24)) {
          std::error_code removeError;
          std::filesystem::remove(entryPath, removeError);
        }
      } else if (!ec && entryPath.extension() == kCacheExtension) {
        const uint64_t size = entry.file_size(ec);
        if (!ec) {
          files.push_back(File{entry.path(), size, modified});
          totalBytes = size > (std::numeric_limits<uint64_t>::max)() - totalBytes
                           ? (std::numeric_limits<uint64_t>::max)()
                           : totalBytes + size;
        }
      }
    }
    ec.clear();
    iterator.increment(ec);
  }
  if (totalBytes <= impl_->config.persistentByteLimit) return;
  std::sort(files.begin(), files.end(), [](const File& left, const File& right) {
    return left.modified < right.modified;
  });
  for (const File& file : files) {
    if (totalBytes <= impl_->config.persistentByteLimit) break;
    ec.clear();
    if (std::filesystem::remove(file.path, ec) && !ec) {
      totalBytes = file.bytes > totalBytes ? 0 : totalBytes - file.bytes;
    }
  }
}

size_t Cache::memoryBytes() const { return impl_->bytes; }

size_t Cache::memoryFrames() const { return impl_->entries.size(); }

std::filesystem::path Cache::persistentDirectory() const {
  return impl_->directory;
}

}  // namespace playback_video_timeline_preview
