#include <iostream>
#include <optional>
#include <vector>

#include "browser_model.h"
#include "browser_playback_source.h"
#include "playback_queue.h"

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "playback_queue_tests: " << message << '\n';
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
  playback_queue::Queue queue(
      [&](const std::filesystem::path& file) -> std::optional<PlaybackTarget> {
        ++resolveCalls;
        if (samePath(file, skipped)) {
          return std::nullopt;
        }
        return PlaybackTarget{file, 0};
      });

  std::vector<std::filesystem::path> requestedFiles{songA, skipped, songB,
                                                    songC};
  playback_queue::Source files = playback_queue::fromFiles(requestedFiles);
  requestedFiles.clear();
  ok &= expect(queue.activate(std::move(files), {songB, 0}),
               "a source containing the current target must activate");
  ok &= expect(resolveCalls == 0,
               "activating a queue must not scan or resolve its items");
  ok &= expect(
      isTarget(queue.adjacent(playback_queue::Direction::Previous), songA, 0),
      "previous must skip an unresolvable source item");
  ok &= expect(
      isTarget(queue.adjacent(playback_queue::Direction::Next), songC, 0),
      "next must resolve lazily in stable source order");

  ok &= expect(
      !queue.activate(playback_queue::fromFiles({unrelated}), {songB, 0}),
      "a source missing the current target must be rejected");
  ok &= expect(
      isTarget(queue.adjacent(playback_queue::Direction::Next), songC, 0),
      "a rejected activation must leave the active queue intact");
  ok &= expect(
      !queue.select({unrelated, 0}) &&
          isTarget(queue.adjacent(playback_queue::Direction::Next), songC, 0),
      "an unknown route target must not create an implicit singleton queue");

  std::vector<BrowserEntry> trackEntries;
  trackEntries.emplace_back("Info", songA, browser_entry::Information{});
  trackEntries.emplace_back("Track 1", songA, browser_entry::PlayTrack{0});
  trackEntries.emplace_back("Track 4", songA, browser_entry::PlayTrack{3});
  trackEntries.emplace_back("Track 9", songA, browser_entry::PlayTrack{8});
  trackEntries.emplace_back("Folder", "C:/Media/Sub",
                            browser_entry::OpenDirectory{});
  playback_queue::Source browserSource =
      browser_playback_source::capture(trackEntries);
  ok &= expect(queue.activate(std::move(browserSource), {songA, 3}),
               "an exact track-browser target must activate");
  const int resolvesBeforeExactTransport = resolveCalls;
  ok &= expect(
      isTarget(queue.adjacent(playback_queue::Direction::Previous), songA, 0) &&
          isTarget(queue.adjacent(playback_queue::Direction::Next), songA, 8),
      "track-browser transport must preserve exact track order");
  ok &= expect(resolveCalls == resolvesBeforeExactTransport,
               "exact track targets must not invoke the path resolver");

  ok &= expect(queue.activate(playback_queue::single({songC, 2}), {songC, 2}),
               "a caller must be able to choose an explicit singleton queue");
  ok &= expect(!queue.adjacent(playback_queue::Direction::Previous) &&
                   !queue.adjacent(playback_queue::Direction::Next),
               "an explicit singleton queue must have no neighbours");

#ifdef _WIN32
  ok &= expect(
      queue.activate(playback_queue::fromFiles(
                         {std::filesystem::path("C:/Media/A.flac"), songB}),
                     {std::filesystem::path("c:\\media\\a.FLAC"), 0}),
      "Windows activation must use ordinal path identity");
  ok &= expect(
      isTarget(queue.adjacent(playback_queue::Direction::Next), songB, 0),
      "Windows transport must preserve path-identity matching");
#endif

  return ok ? 0 : 1;
}
