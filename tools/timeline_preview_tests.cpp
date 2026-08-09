#include "playback/video/decoder.h"
#include "playback/video/timeline_preview.h"
#include "playback/video/timeline_preview_model.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "timeline_preview_tests: " << message << '\n';
    return false;
  }
  return true;
}

std::filesystem::path uniqueTestDirectory(const char* label) {
  std::error_code ec;
  const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
  if (ec) return {};
  return base / (std::string("radioify-") + label + "-" +
                 std::to_string(GetCurrentProcessId()) + "-" +
                 std::to_string(GetTickCount64()));
}

size_t thumbnailFileCount(const std::filesystem::path& root) {
  std::error_code ec;
  size_t count = 0;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, ec);
  const std::filesystem::recursive_directory_iterator end;
  while (!ec && iterator != end) {
    if (iterator->is_regular_file(ec) && !ec &&
        iterator->path().extension() == L".png") {
      ++count;
    }
    ec.clear();
    iterator.increment(ec);
  }
  return count;
}

bool hasPngSignature(const std::filesystem::path& path) {
  constexpr std::array<uint8_t, 8> signature = {
      0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  std::array<uint8_t, signature.size()> bytes{};
  std::ifstream stream(path, std::ios::binary);
  stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  return stream.good() && bytes == signature;
}

bool runPolicyTests() {
  using namespace playback_video_timeline_preview;
  bool ok = true;

  ok &= expect(bucketDurationUs(10'000'000, 200) == 250'000,
               "short media must retain the minimum useful bucket");
  ok &= expect(bucketDurationUs(60'000'000, 120) == 500'000,
               "bucket size must follow progress-bar addressability");
  ok &= expect(bucketDurationUs(36'000'000'000, 24) == 10'000'000,
               "very long media must retain bounded cache granularity");
  ok &= expect(bucketTargetUs(0, 60'000'000, 500'000) == 0,
               "the beginning of media must resolve to its first frame");
  ok &= expect(bucketTargetUs(60'000'000, 60'000'000, 500'000) ==
                   59'999'999,
               "the end of media must remain inside its duration");

  const auto backward =
      prefetchTargets(30'000'000, 500'000, 60'000'000, -1);
  ok &= expect(backward.size() == 3 && backward[0] == 29'500'000 &&
                   backward[1] == 29'000'000 &&
                   backward[2] == 30'500'000,
               "backward scrub prefetch must prioritize earlier neighbors");
  const auto forward =
      prefetchTargets(30'000'000, 500'000, 60'000'000, 1);
  ok &= expect(forward.size() == 3 && forward[0] == 30'500'000 &&
                   forward[1] == 31'000'000 &&
                   forward[2] == 29'500'000,
               "forward scrub prefetch must prioritize later neighbors");

  const auto landscape = fitDecodeSize(1920, 1080);
  ok &= expect(landscape.first == 320 && landscape.second == 180,
               "16:9 previews must fit the decode budget exactly");
  const auto portrait = fitDecodeSize(1080, 1920);
  ok &= expect(portrait.first <= 320 && portrait.second == 180 &&
                   (portrait.first % 2) == 0,
               "portrait previews must preserve aspect and even dimensions");
  const auto smallPreview = fitDecodeSize(160, 90);
  ok &= expect(smallPreview.first == 160 && smallPreview.second == 90,
               "small sources must not be needlessly upscaled");

  ok &= expect(formatTimestamp(83'400'000) == "1:23",
               "sub-hour labels must use M:SS");
  ok &= expect(formatTimestamp(3'723'000'000) == "1:02:03",
               "long labels must include hours");

  const CellLayout left = layoutCells(120, 40, 34, 1, 118, 0.0, 1920,
                                      1080, 9.0, 21.0, "0:00");
  const CellLayout right = layoutCells(120, 40, 34, 1, 118, 1.0, 1920,
                                       1080, 9.0, 21.0, "1:00");
  ok &= expect(left.drawable() && right.drawable(),
               "normal terminal geometry must produce a preview popup");
  ok &= expect(left.outerX == 0 &&
                   right.outerX + right.outerWidth <= 120,
               "preview popups must clamp at both progress-bar edges");
  ok &= expect(left.outerY + left.outerHeight < 34,
               "preview popup must not overlap the controls footer");
  ok &= expect(!layoutCells(8, 5, 4, 0, 8, 0.5, 16, 9, 9.0, 21.0,
                            "0:00")
                    .drawable(),
               "tiny surfaces must decline an unusable preview");
  return ok;
}

bool runModelTests() {
  using namespace playback_video_timeline_preview;
  bool ok = true;
  HoverModel model;
  model.start(60'000'000);

  HoverModel::Update first = model.hover(0.5, 120);
  ok &= expect(first.changed && first.request.has_value(),
               "first hover must create an exact provider request");
  if (!first.request) return false;
  ok &= expect(model.snapshot().hoverActive &&
                   !model.snapshot().hasImage(),
               "pending hover must expose timestamp intent without an image");
  const uint64_t firstId = first.request->id;
  const int64_t decodeTargetUs = first.request->targetUs;
  HoverModel::Update sameBucket = model.hover(0.5001, 120);
  ok &= expect(sameBucket.changed && !sameBucket.request,
               "cursor motion inside one bucket must only update the view");

  HoverModel::Update resized = model.hover(0.5001, 60);
  ok &= expect(resized.changed && resized.request.has_value(),
               "changed progress geometry must refresh the prefetch spacing");
  if (!resized.request) return false;
  const uint64_t resizedId = resized.request->id;
  const int64_t resizedTargetUs = resized.request->targetUs;

  ok &= expect(!model.reject(*first.request),
               "the model must ignore stale provider rejections");

  auto image = std::make_shared<Image>();
  image->id = 1;
  image->requestedUs = resizedTargetUs;
  image->surface.width = 1;
  image->surface.height = 1;
  image->surface.strideBytes = 4;
  image->surface.pixels = {20, 40, 60, 255};
  ok &= expect(!model.apply(Result{firstId, decodeTargetUs,
                                  ResultOrigin::Decoded, image}),
               "the model must reject stale provider results");
  ok &= expect(model.apply(Result{resizedId, resizedTargetUs,
                                 ResultOrigin::Decoded, image}),
               "the model must accept its authoritative result");
  const Snapshot ready = model.snapshot();
  ok &= expect(ready.hoverActive && ready.hasImage() && ready.image == image,
               "an accepted result must become the visible snapshot");

  HoverModel::Update rejectedUpdate = model.hover(0.7, 120);
  ok &= expect(rejectedUpdate.request.has_value(),
               "a new bucket must create a provider request");
  if (!rejectedUpdate.request) return false;
  ok &= expect(model.reject(*rejectedUpdate.request),
               "the model must own current submission rejection state");
  const Snapshot rejected = model.snapshot();
  ok &= expect(rejected.hoverActive && !rejected.hasImage(),
               "a rejected request must leave only timestamp presentation");
  ok &= expect(!model.apply(Result{rejectedUpdate.request->id,
                                  rejectedUpdate.request->targetUs,
                                  ResultOrigin::Decoded, image}),
               "submission rejection must invalidate any late completion");

  ok &= expect(model.hide(), "hiding an active preview must change the model");
  ok &= expect(!model.apply(Result{resizedId, resizedTargetUs,
                                  ResultOrigin::Decoded, image}),
               "a hidden model must reject late decoder completion");
  ok &= expect(!model.snapshot().hoverActive,
               "hide must withdraw the presentation snapshot");
  return ok;
}

std::shared_ptr<const playback_video_timeline_preview::Image> testImage(
    uint64_t id, int64_t targetUs) {
  using namespace playback_video_timeline_preview;
  auto image = std::make_shared<Image>();
  image->id = id;
  image->requestedUs = targetUs;
  image->decodedFrameUs = targetUs;
  image->surface.width = 4;
  image->surface.height = 4;
  image->surface.strideBytes = 16;
  image->surface.pixels.resize(64);
  const uint8_t base = static_cast<uint8_t>(targetUs & 0xff);
  for (uint32_t y = 0; y < image->surface.height; ++y) {
    for (uint32_t x = 0; x < image->surface.width; ++x) {
      const size_t index = static_cast<size_t>(y) * image->surface.strideBytes +
                           static_cast<size_t>(x) * 4u;
      image->surface.pixels[index + 0u] =
          static_cast<uint8_t>(base + x * 17u + y);
      image->surface.pixels[index + 1u] =
          static_cast<uint8_t>(base + x + y * 23u);
      image->surface.pixels[index + 2u] =
          static_cast<uint8_t>(base + x * 7u + y * 11u);
      image->surface.pixels[index + 3u] =
          static_cast<uint8_t>(64u + x * 16u + y * 8u);
    }
  }
  return image;
}

bool runCacheTests(const std::filesystem::path& identityFile) {
  using namespace playback_video_timeline_preview;
  const std::filesystem::path root = uniqueTestDirectory("preview-cache");
  if (root.empty()) return expect(false, "temporary cache root unavailable");
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (ec) return expect(false, "temporary cache root could not be created");

  CacheConfig config;
  config.memoryByteLimit = 64;
  config.memoryFrameLimit = 1;
  config.persistentByteLimit = 4 * 1024 * 1024;
  config.persistentRoot = root;
  Source source{identityFile, 0, 60'000'000, 1920, 1080};
  bool ok = true;
  {
    Cache cache(config);
    ok &= expect(cache.open(source), "cache must accept a valid media identity");
    const auto firstImage = testImage(1, 1'000'000);
    cache.storeMemory(firstImage);
    cache.storePersistent(firstImage);
    ok &= expect(hasPngSignature(cache.persistentDirectory() / "1000000.png"),
                 "persistent thumbnails must use the standard PNG format");
    CacheLookup memory = cache.find(1'000'000, 99);
    ok &= expect(memory.image && memory.origin == ResultOrigin::MemoryCache,
                 "fresh exact entries must hit the volatile LRU");
    const auto secondImage = testImage(2, 2'000'000);
    cache.storeMemory(secondImage);
    cache.storePersistent(secondImage);
    ok &= expect(cache.memoryFrames() == 1 && cache.memoryBytes() == 64,
                 "volatile cache must enforce byte and frame budgets");
    ok &= expect(!cache.find(1'000'001, 100).image,
                 "cache lookup must never substitute a nearby timestamp");
    CacheLookup disk = cache.find(1'000'000, 101);
    ok &= expect(disk.image &&
                     disk.origin == ResultOrigin::PersistentCache &&
                     disk.image->id == 101 &&
                     !disk.image->decodedFrameUs.has_value() &&
                     disk.image->surface.pixels ==
                         firstImage->surface.pixels,
                 "an evicted exact RGBA entry must reload losslessly");

    auto overBudget = std::make_shared<Image>(*secondImage);
    overBudget->requestedUs = 2'500'000;
    overBudget->surface.pixels.push_back(0);
    cache.storeMemory(overBudget);
    ok &= expect(!cache.find(2'500'000, 104).image &&
                     cache.memoryBytes() == 64,
                 "the volatile budget must count the actual allocation");

    auto invalid = std::make_shared<Image>(*testImage(3, 3'000'000));
    invalid->surface.pixels.pop_back();
    cache.storeMemory(invalid);
    cache.storePersistent(invalid);
    ok &= expect(!cache.find(3'000'000, 103).image,
                 "incomplete RGBA surfaces must never enter either cache tier");

    const std::filesystem::path corruptPng =
        cache.persistentDirectory() / "3500000.png";
    {
      std::ofstream stream(corruptPng, std::ios::binary);
      stream << "not a PNG";
    }
    ok &= expect(!cache.find(3'500'000, 105).image &&
                     !std::filesystem::exists(corruptPng),
                 "a corrupt persistent image must be rejected and removed");

    const std::filesystem::path staleTemporary =
        cache.persistentDirectory() / "orphan.png.tmp-1-1";
    {
      std::ofstream stream(staleTemporary, std::ios::binary);
      stream << "orphan";
    }
    ec.clear();
    std::filesystem::last_write_time(
        staleTemporary,
        std::filesystem::file_time_type::clock::now() -
            std::chrono::hours(25),
        ec);
    cache.prunePersistent();
    ok &= expect(!ec && !std::filesystem::exists(staleTemporary),
                 "maintenance must remove abandoned atomic-write files");
  }
  {
    Cache reopened(config);
    ok &= expect(reopened.open(source),
                 "persistent cache must reopen for the same media identity");
    CacheLookup disk = reopened.find(2'000'000, 102);
    ok &= expect(disk.image && disk.origin == ResultOrigin::PersistentCache,
                 "persistent entries must survive provider recreation");
  }
  {
    CacheConfig tinyBudget = config;
    tinyBudget.persistentByteLimit = 1;
    Cache pruner(tinyBudget);
    ok &= expect(pruner.open(source),
                 "maintenance cache must reopen the media namespace");
    pruner.prunePersistent();
    ok &= expect(thumbnailFileCount(root) == 0,
                 "persistent maintenance must enforce its global byte cap");
  }
  ec.clear();
  std::filesystem::remove_all(root, ec);
  ok &= expect(!ec, "temporary cache data must clean up cleanly");
  return ok;
}

struct MediaSample {
  bool ready = false;
  int64_t elapsedMs = 0;
  std::optional<int64_t> frameOffsetUs;
  playback_video_timeline_preview::ResultOrigin origin =
      playback_video_timeline_preview::ResultOrigin::Decoded;
};

MediaSample waitForMediaResult(
    playback_video_timeline_preview::Provider& provider,
    playback_video_timeline_preview::HoverModel& model, uint64_t requestId,
    int64_t expectedDecodeUs, std::chrono::steady_clock::time_point startedAt) {
  using namespace playback_video_timeline_preview;
  MediaSample sample;
  while (std::chrono::steady_clock::now() - startedAt <
         std::chrono::seconds(10)) {
    if (std::optional<Result> result = provider.takeResult()) {
      sample.origin = result->origin;
      model.apply(*result);
    }
    const Snapshot snapshot = model.snapshot();
    if (snapshot.hoverActive && snapshot.hasImage() &&
        snapshot.image->requestedUs == expectedDecodeUs &&
        requestId == model.requestId()) {
      if (snapshot.image->decodedFrameUs) {
        sample.frameOffsetUs =
            *snapshot.image->decodedFrameUs - expectedDecodeUs;
      }
      const bool timestampValid =
          sample.origin == ResultOrigin::PersistentCache
              ? !snapshot.image->decodedFrameUs.has_value()
              : sample.frameOffsetUs.has_value() &&
                    std::llabs(*sample.frameOffsetUs) <= 250'000;
      sample.ready = snapshot.image->surface.width > 0 &&
                     snapshot.image->surface.width <= kDecodeMaxWidth &&
                     snapshot.image->surface.height > 0 &&
                     snapshot.image->surface.height <= kDecodeMaxHeight &&
                     playback_video_image::validate(snapshot.image->surface) &&
                     timestampValid;
      break;
    }
    WaitForSingleObject(provider.changedWaitHandle().get(), 250);
  }
  sample.elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - startedAt)
          .count();
  return sample;
}

bool runMediaSmoke(const std::filesystem::path& path) {
  using namespace playback_video_timeline_preview;
  VideoDecoder probe;
  VideoStreamSelection selection;
  std::string error;
  if (!probe.init(path, &error, false, true, &selection)) {
    std::cerr << "timeline_preview_tests: media probe failed: " << error
              << '\n';
    return false;
  }
  const Source source{path, selection.selectedIndex,
                      probe.duration100ns() / 10, probe.width(),
                      probe.height()};
  probe.uninit();
  if (source.sourceWidth <= 0 || source.sourceHeight <= 0 ||
      source.durationUs <= 0 || source.videoStreamIndex < 0) {
    std::cerr << "timeline_preview_tests: media probe returned invalid metadata\n";
    return false;
  }

  const std::filesystem::path root = uniqueTestDirectory("preview-media");
  CacheConfig cacheConfig;
  cacheConfig.persistentRoot = root;
  bool ok = true;
  int64_t expectedDecodeUs = 0;
  MediaSample decoded;
  MediaSample endpoint;
  {
    HoverModel model;
    Provider provider(cacheConfig);
    model.start(source.durationUs);
    if (!provider.start(source)) {
      std::cerr << "timeline_preview_tests: preview provider failed to start\n";
      return false;
    }
    const auto startedAt = std::chrono::steady_clock::now();
    for (const double ratio : {0.10, 0.90, 0.50}) {
      HoverModel::Update update = model.hover(ratio, 160);
      if (!update.request || !provider.submit(*update.request)) {
        provider.stop();
        return expect(false, "provider must accept authoritative hover work");
      }
      expectedDecodeUs = update.request->targetUs;
      if (ratio != 0.50) Sleep(15);
    }
    decoded = waitForMediaResult(provider, model, model.requestId(),
                                 expectedDecodeUs, startedAt);
    HoverModel::Update endpointUpdate = model.hover(1.0, 160);
    if (!endpointUpdate.request || !provider.submit(*endpointUpdate.request)) {
      provider.stop();
      return expect(false, "provider must accept an end-of-media request");
    }
    const auto endpointStartedAt = std::chrono::steady_clock::now();
    endpoint = waitForMediaResult(provider, model, endpointUpdate.request->id,
                                  endpointUpdate.request->targetUs,
                                  endpointStartedAt);
    const bool hidden = model.hide();
    provider.cancelBefore(model.requestId());
    provider.stop();
    ok &= expect(hidden && !model.snapshot().hoverActive,
                 "hiding a preview must withdraw its snapshot");
  }

  MediaSample persisted;
  {
    HoverModel model;
    Provider provider(cacheConfig);
    model.start(source.durationUs);
    if (!provider.start(source)) return false;
    HoverModel::Update update = model.hover(0.50, 160);
    if (!update.request || !provider.submit(*update.request)) return false;
    const auto startedAt = std::chrono::steady_clock::now();
    persisted = waitForMediaResult(provider, model, update.request->id,
                                   update.request->targetUs, startedAt);
    provider.stop();
  }

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  const std::string decodedOffset =
      decoded.frameOffsetUs ? std::to_string(*decoded.frameOffsetUs) : "n/a";
  std::cout << "timeline_preview_media decode_ms=" << decoded.elapsedMs
            << " frame_offset_us=" << decodedOffset
            << " endpoint_ms=" << endpoint.elapsedMs
            << " persistent_ms=" << persisted.elapsedMs
            << " persistent_hit="
            << (persisted.origin == ResultOrigin::PersistentCache ? 1 : 0)
            << '\n';
  return ok &&
         expect(decoded.ready,
                "latest hover request must publish a bounded exact frame") &&
         expect(endpoint.ready,
                "end hover must resolve only after natural decoder EOF") &&
         expect(persisted.ready &&
                    persisted.origin == ResultOrigin::PersistentCache,
                "a reopened provider must reuse the persistent exact frame") &&
         expect(!ec, "media smoke cache must clean up cleanly");
}

}  // namespace

int main(int argc, char** argv) {
  bool ok = runPolicyTests();
  ok &= runModelTests();
  ok &= runCacheTests(std::filesystem::path(argv[0]));
  if (argc >= 2) ok &= runMediaSmoke(std::filesystem::path(argv[1]));
  return ok ? 0 : 1;
}
