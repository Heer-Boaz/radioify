#include "playback/video/timeline_preview.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "timeline_preview_tests: " << message << '\n';
    return false;
  }
  return true;
}

bool runPolicyTests() {
  using namespace playback_video_timeline_preview;
  bool ok = true;

  ok &= expect(bucketDurationUs(10'000'000, 200) == 250'000,
               "short media must retain the minimum useful bucket");
  ok &= expect(bucketDurationUs(60'000'000, 120) == 500'000,
               "bucket size must follow progress-bar addressability");
  ok &= expect(bucketDurationUs(36'000'000'000, 24) == 10'000'000,
               "very long media must retain the bounded cache granularity");
  ok &= expect(bucketTargetUs(0, 60'000'000, 500'000) == 0,
               "the beginning of media must resolve to its first frame");
  ok &= expect(bucketTargetUs(60'000'000, 60'000'000, 500'000) ==
                   59'999'999,
               "the end of media must remain inside its duration");

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
  const int width = probe.width();
  const int height = probe.height();
  const int64_t durationUs = probe.duration100ns() / 10;
  const int streamIndex = selection.selectedIndex;
  probe.uninit();
  if (width <= 0 || height <= 0 || durationUs <= 0 || streamIndex < 0) {
    std::cerr << "timeline_preview_tests: media probe returned invalid metadata\n";
    return false;
  }

  Service service;
  if (!service.start(path, streamIndex, durationUs, width, height)) {
    std::cerr << "timeline_preview_tests: preview service failed to start\n";
    return false;
  }

  const auto startedAt = std::chrono::steady_clock::now();
  service.request(0.10, 160);
  Sleep(15);
  service.request(0.90, 160);
  Sleep(15);
  service.request(0.50, 160);
  const int64_t expectedTargetUs =
      static_cast<int64_t>(std::llround(0.50 * (durationUs - 1)));
  bool ready = false;
  int64_t frameOffsetUs = 0;
  while (std::chrono::steady_clock::now() - startedAt <
         std::chrono::seconds(10)) {
    const Snapshot snapshot = service.snapshot();
    if (snapshot.visible && !snapshot.loading && snapshot.image &&
        snapshot.targetUs == expectedTargetUs) {
      const int64_t expectedDecodeUs = bucketTargetUs(
          expectedTargetUs, durationUs, bucketDurationUs(durationUs, 160));
      frameOffsetUs = snapshot.image->frameUs - expectedDecodeUs;
      ready = snapshot.image->frame.width > 0 &&
              snapshot.image->frame.width <= kDecodeMaxWidth &&
              snapshot.image->frame.height > 0 &&
              snapshot.image->frame.height <= kDecodeMaxHeight &&
              snapshot.image->frame.storageBytes > 0 &&
              snapshot.image->requestedUs == expectedDecodeUs &&
              std::llabs(frameOffsetUs) <= 250'000;
      break;
    }
    WaitForSingleObject(service.changedWaitHandle().get(), 250);
    service.consumeChanged();
  }
  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - startedAt)
                             .count();
  std::cout << "timeline_preview_media latest_request_ms=" << elapsedMs
            << " frame_offset_us=" << frameOffsetUs
            << " ready=" << (ready ? 1 : 0) << '\n';
  const bool hidden = service.hide() && !service.snapshot().visible;
  service.stop();
  return expect(ready, "latest hover request must publish a bounded frame") &&
         expect(hidden, "hiding a preview must withdraw its snapshot");
}

}  // namespace

int main(int argc, char** argv) {
  bool ok = runPolicyTests();
  if (argc >= 2) {
    ok &= runMediaSmoke(std::filesystem::path(argv[1]));
  }
  return ok ? 0 : 1;
}
