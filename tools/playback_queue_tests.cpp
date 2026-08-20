#include <iostream>
#include <utility>
#include <vector>

#include "app/playback_queue.h"
#include "browser_model.h"
#include "browser_playback_source.h"

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "playback_queue_tests: " << message << '\n';
    return false;
  }
  return true;
}

playback_route::Route routeFor(const PlaybackTarget& target) {
  playback_route::Route route;
  route.target = target;
  return route;
}

bool isTarget(const PlaybackTarget& actual, const std::filesystem::path& file,
              int trackIndex) {
  return samePath(actual.file, file) && actual.trackIndex == trackIndex;
}

}  // namespace

int main() {
  bool ok = true;
  const std::filesystem::path songA = "C:/Media/A.flac";
  const std::filesystem::path skipped = "C:/Media/cover.txt";
  const std::filesystem::path songB = "C:/Media/B.flac";
  const std::filesystem::path songC = "C:/Media/C.flac";
  const std::filesystem::path cover = "C:/Media/cover.jpg";
  const std::filesystem::path unrelated = "C:/Elsewhere/X.flac";

  int resolveCalls = 0;
  playback_queue::Queue queue(
      {[&](const std::filesystem::path& file) -> std::optional<PlaybackTarget> {
         ++resolveCalls;
         if (samePath(file, skipped)) {
           return std::nullopt;
         }
         return PlaybackTarget{file, 0};
       },
       [](const PlaybackTarget& target) { return routeFor(target); }});

  std::vector<std::filesystem::path> requestedFiles{songA, skipped, songB,
                                                    songC};
  playback_queue::Source files =
      playback_queue::sourceFromFiles(requestedFiles);
  requestedFiles.clear();
  std::optional<playback_queue::Queue::PreparedActivation> initial =
      queue.prepareStart(routeFor({songB, 0}), std::move(files));
  ok &= expect(initial && isTarget(initial->route().target, songB, 0),
               "a source containing its target must prepare");
  ok &= expect(resolveCalls == 0,
               "preparing a source must not eagerly resolve its items");
  ok &= expect(!queue.prepareTransport(playback_queue::Direction::Previous),
               "transport must remain unavailable before activation commits");
  queue.commit(std::move(*initial));

  std::optional<playback_queue::Queue::PreparedActivation> previous =
      queue.prepareTransport(playback_queue::Direction::Previous);
  ok &= expect(previous && isTarget(previous->route().target, songA, 0),
               "previous must lazily skip an unresolvable source item");
  ok &= expect(resolveCalls > 0,
               "transport must resolve path-only items at the playback owner");

  std::optional<playback_queue::Queue::PreparedActivation> nextBeforeCommit =
      queue.prepareTransport(playback_queue::Direction::Next);
  ok &= expect(
      nextBeforeCommit && isTarget(nextBeforeCommit->route().target, songC, 0),
      "preparation must not mutate the committed position");

  queue.commit(std::move(*previous));
  std::optional<playback_queue::Queue::PreparedActivation> next =
      queue.prepareTransport(playback_queue::Direction::Next);
  ok &= expect(next && isTarget(next->route().target, songB, 0),
               "transport must advance from the committed position");
  queue.commit(std::move(*next));

  ok &=
      expect(!queue.prepareStart(routeFor({songB, 0}),
                                 playback_queue::sourceFromFiles({unrelated})),
             "a source missing its requested target must be rejected");
  std::optional<playback_queue::Queue::PreparedActivation>
      nextAfterRejectedSource =
          queue.prepareTransport(playback_queue::Direction::Next);
  ok &= expect(nextAfterRejectedSource &&
                   isTarget(nextAfterRejectedSource->route().target, songC, 0),
               "a rejected source must leave the active source intact");

  std::optional<playback_queue::Queue::PreparedActivation>
      discardedReplacement =
          queue.prepareStart(routeFor({unrelated, 0}),
                             playback_queue::singleSource({unrelated, 0}));
  ok &= expect(discardedReplacement &&
                   isTarget(discardedReplacement->route().target, unrelated, 0),
               "a valid replacement source must prepare independently");
  discardedReplacement.reset();
  std::optional<playback_queue::Queue::PreparedActivation> nextAfterDiscard =
      queue.prepareTransport(playback_queue::Direction::Next);
  ok &= expect(
      nextAfterDiscard && isTarget(nextAfterDiscard->route().target, songC, 0),
      "discarding a prepared source must preserve active playback");

  std::vector<BrowserEntry> trackEntries;
  trackEntries.emplace_back("Info", songA, browser_entry::Information{});
  trackEntries.emplace_back("Track 1", songA, browser_entry::PlayTrack{0});
  trackEntries.emplace_back("Track 4", songA, browser_entry::PlayTrack{3});
  trackEntries.emplace_back("Cover", cover, browser_entry::OpenFile{});
  trackEntries.emplace_back("Track 9", songA, browser_entry::PlayTrack{8});
  trackEntries.emplace_back("Folder", "C:/Media/Sub",
                            browser_entry::OpenDirectory{});
  const int resolvesBeforeExactTransport = resolveCalls;
  std::optional<playback_queue::Queue::PreparedActivation> exactTrack =
      queue.prepareStart(routeFor({songA, 3}),
                         browser_playback_source::capture(trackEntries));
  ok &= expect(exactTrack.has_value(),
               "an exact track-browser source must prepare");
  queue.commit(std::move(*exactTrack));
  std::optional<playback_queue::Queue::PreparedActivation> previousTrack =
      queue.prepareTransport(playback_queue::Direction::Previous);
  ok &=
      expect(previousTrack && isTarget(previousTrack->route().target, songA, 0),
             "track-browser previous must preserve exact track order");

  exactTrack = queue.prepareStart(
      routeFor({songA, 3}), browser_playback_source::capture(trackEntries));
  queue.commit(std::move(*exactTrack));
  std::optional<playback_queue::Queue::PreparedActivation> nextTrack =
      queue.prepareTransport(playback_queue::Direction::Next);
  ok &= expect(nextTrack && isTarget(nextTrack->route().target, songA, 8),
               "track-browser next must preserve exact track order");
  ok &= expect(resolveCalls == resolvesBeforeExactTransport,
               "exact track targets must not invoke the path resolver");

  std::optional<playback_queue::Queue::PreparedActivation> singleton =
      queue.prepareStart(routeFor({songC, 2}),
                         playback_queue::singleSource({songC, 2}));
  ok &= expect(singleton.has_value(),
               "a direct action must prepare an explicit singleton source");
  queue.commit(std::move(*singleton));
  ok &= expect(!queue.prepareTransport(playback_queue::Direction::Previous) &&
                   !queue.prepareTransport(playback_queue::Direction::Next),
               "an explicit singleton source must have no neighbours");

#ifdef _WIN32
  std::optional<playback_queue::Queue::PreparedActivation> windowsPath =
      queue.prepareStart(
          routeFor({std::filesystem::path("c:\\media\\a.FLAC"), 0}),
          playback_queue::sourceFromFiles(
              {std::filesystem::path("C:/Media/A.flac"), songB}));
  ok &= expect(windowsPath.has_value(),
               "Windows source matching must use ordinal path identity");
  queue.commit(std::move(*windowsPath));
  std::optional<playback_queue::Queue::PreparedActivation> windowsNext =
      queue.prepareTransport(playback_queue::Direction::Next);
  ok &= expect(windowsNext && isTarget(windowsNext->route().target, songB, 0),
               "Windows transport must preserve path-identity matching");
#endif

  return ok ? 0 : 1;
}
