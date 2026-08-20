#include <iostream>
#include <optional>
#include <utility>
#include <vector>

#include "app/playback_controller.h"
#include "browser_model.h"
#include "browser_playback_source.h"

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "playback_controller_tests: " << message << '\n';
    return false;
  }
  return true;
}

bool isTarget(const std::optional<PlaybackTarget>& actual,
              const std::filesystem::path& file, int trackIndex) {
  return actual && samePath(actual->file, file) &&
         actual->trackIndex == trackIndex;
}

}  // namespace

int main() {
  bool ok = true;
  const std::filesystem::path songA = "C:/Media/A.flac";
  const std::filesystem::path skipped = "C:/Media/cover.txt";
  const std::filesystem::path songB = "C:/Media/B.flac";
  const std::filesystem::path songC = "C:/Media/C.flac";
  const std::filesystem::path unrelated = "C:/Elsewhere/X.flac";

  int resolveCalls = 0;
  playback_controller::Controller controller(
      [&](const std::filesystem::path& file) -> std::optional<PlaybackTarget> {
        ++resolveCalls;
        if (samePath(file, skipped)) {
          return std::nullopt;
        }
        return PlaybackTarget{file, 0};
      });

  std::vector<std::filesystem::path> requestedFiles{songA, skipped, songB,
                                                    songC};
  playback_controller::Source files =
      playback_controller::sourceFromFiles(requestedFiles);
  requestedFiles.clear();
  ok &= expect(controller.apply(
                   playback_controller::start(std::move(files), {songB, 0})),
               "a start transition containing its target must apply");
  ok &= expect(resolveCalls == 0,
               "starting playback must not eagerly resolve source items");
  ok &= expect(
      isTarget(controller.adjacent(playback_controller::Direction::Previous),
               songA, 0),
      "previous must skip an unresolvable source item");
  ok &=
      expect(isTarget(controller.adjacent(playback_controller::Direction::Next),
                      songC, 0),
             "next must resolve lazily in stable source order");
  ok &= expect(
      controller.apply(playback_controller::continueWith({songC, 0})) &&
          isTarget(controller.adjacent(playback_controller::Direction::Previous),
                   songB, 0),
      "a continuation transition must advance the controller-owned position");
  ok &= expect(
      controller.apply(playback_controller::continueWith({songB, 0})),
      "a valid continuation must be able to restore the active position");

  ok &= expect(
      !controller.apply(playback_controller::start(
          playback_controller::sourceFromFiles({unrelated}), {songB, 0})),
      "a start transition missing its target must be rejected");
  ok &=
      expect(isTarget(controller.adjacent(playback_controller::Direction::Next),
                      songC, 0),
             "a rejected start must leave the active source intact");
  ok &= expect(
      !controller.apply(playback_controller::continueWith({unrelated, 0})) &&
          isTarget(controller.adjacent(playback_controller::Direction::Next),
                   songC, 0),
      "an invalid continuation must not invent a singleton source");

  std::vector<BrowserEntry> trackEntries;
  trackEntries.emplace_back("Info", songA, browser_entry::Information{});
  trackEntries.emplace_back("Track 1", songA, browser_entry::PlayTrack{0});
  trackEntries.emplace_back("Track 4", songA, browser_entry::PlayTrack{3});
  trackEntries.emplace_back("Track 9", songA, browser_entry::PlayTrack{8});
  trackEntries.emplace_back("Folder", "C:/Media/Sub",
                            browser_entry::OpenDirectory{});
  playback_controller::Source browserSource =
      browser_playback_source::capture(trackEntries);
  ok &= expect(controller.apply(playback_controller::start(
                   std::move(browserSource), {songA, 3})),
               "an exact track-browser start must apply");
  const int resolvesBeforeExactTransport = resolveCalls;
  ok &= expect(
      isTarget(controller.adjacent(playback_controller::Direction::Previous),
               songA, 0) &&
          isTarget(controller.adjacent(playback_controller::Direction::Next),
                   songA, 8),
      "track-browser transport must preserve exact track order");
  ok &= expect(resolveCalls == resolvesBeforeExactTransport,
               "exact track targets must not invoke the path resolver");

  ok &= expect(controller.apply(playback_controller::start(
                   playback_controller::singleSource({songC, 2}), {songC, 2})),
               "a direct action must request an explicit singleton source");
  ok &= expect(!controller.adjacent(playback_controller::Direction::Previous) &&
                   !controller.adjacent(playback_controller::Direction::Next),
               "an explicit singleton source must have no neighbours");

#ifdef _WIN32
  ok &= expect(controller.apply(playback_controller::start(
                   playback_controller::sourceFromFiles(
                       {std::filesystem::path("C:/Media/A.flac"), songB}),
                   {std::filesystem::path("c:\\media\\a.FLAC"), 0})),
               "Windows start matching must use ordinal path identity");
  ok &=
      expect(isTarget(controller.adjacent(playback_controller::Direction::Next),
                      songB, 0),
             "Windows transport must preserve path-identity matching");
#endif

  return ok ? 0 : 1;
}
