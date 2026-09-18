#include <fstream>
#include <iostream>
#include <optional>
#include <system_error>
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

bool isTrackTarget(const PlaybackTarget& actual,
                   const std::filesystem::path& file, int trackIndex) {
  const std::optional<int> actualTrackIndex =
      playbackTargetTrackIndex(actual);
  return samePath(playbackTargetFile(actual), file) &&
         actualTrackIndex && *actualTrackIndex == trackIndex;
}

bool isFileTarget(const PlaybackTarget& actual,
                  const std::filesystem::path& file) {
  return samePath(playbackTargetFile(actual), file) &&
         !playbackTargetTrackIndex(actual);
}

PlaybackTarget trackTarget(const std::filesystem::path& file, int trackIndex) {
  return *playbackTrackTarget(file, trackIndex);
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
         return trackTarget(file, 0);
       },
       [](const PlaybackTarget& target) { return routeFor(target); }});

  std::vector<std::filesystem::path> requestedFiles{songA, skipped, songB,
                                                    songC};
  playback_queue::Source files =
      playback_queue::sourceFromFiles(requestedFiles);
  requestedFiles.clear();
  std::optional<playback_queue::Queue::PreparedActivation> initial =
      queue.prepareStart(routeFor(trackTarget(songB, 0)), std::move(files));
  ok &= expect(initial && isTrackTarget(initial->route().target, songB, 0),
               "a source containing its target must prepare");
  ok &= expect(resolveCalls == 0,
               "preparing a source must not eagerly resolve its items");
  ok &= expect(!queue.prepareTransport(playback_queue::Direction::Previous),
               "transport must remain unavailable before activation commits");
  queue.commit(std::move(*initial));

  std::optional<playback_queue::Queue::PreparedActivation> previous =
      queue.prepareTransport(playback_queue::Direction::Previous);
  ok &= expect(previous && isTrackTarget(previous->route().target, songA, 0),
               "previous must lazily skip an unresolvable source item");
  ok &= expect(resolveCalls > 0,
               "transport must resolve path-only items at the playback owner");

  std::optional<playback_queue::Queue::PreparedActivation> nextBeforeCommit =
      queue.prepareTransport(playback_queue::Direction::Next);
  ok &= expect(
      nextBeforeCommit &&
          isTrackTarget(nextBeforeCommit->route().target, songC, 0),
      "preparation must not mutate the committed position");

  queue.commit(std::move(*previous));
  std::optional<playback_queue::Queue::PreparedActivation> next =
      queue.prepareTransport(playback_queue::Direction::Next);
  ok &= expect(next && isTrackTarget(next->route().target, songB, 0),
               "transport must advance from the committed position");
  queue.commit(std::move(*next));

  ok &=
      expect(!queue.prepareStart(routeFor(trackTarget(songB, 0)),
                                 playback_queue::sourceFromFiles({unrelated})),
             "a source missing its requested target must be rejected");
  std::optional<playback_queue::Queue::PreparedActivation>
      nextAfterRejectedSource =
          queue.prepareTransport(playback_queue::Direction::Next);
  ok &= expect(nextAfterRejectedSource &&
                   isTrackTarget(nextAfterRejectedSource->route().target,
                                 songC, 0),
               "a rejected source must leave the active source intact");

  std::optional<playback_queue::Queue::PreparedActivation>
      discardedReplacement =
          queue.prepareStart(routeFor(trackTarget(unrelated, 0)),
                             playback_queue::singleSource(
                                 trackTarget(unrelated, 0)));
  ok &= expect(discardedReplacement &&
                   isTrackTarget(discardedReplacement->route().target,
                                 unrelated, 0),
               "a valid replacement source must prepare independently");
  discardedReplacement.reset();
  std::optional<playback_queue::Queue::PreparedActivation> nextAfterDiscard =
      queue.prepareTransport(playback_queue::Direction::Next);
  ok &= expect(
      nextAfterDiscard &&
          isTrackTarget(nextAfterDiscard->route().target, songC, 0),
      "discarding a prepared source must preserve active playback");

  std::vector<BrowserEntry> trackEntries;
  trackEntries.emplace_back("Info", songA, browser_entry::Information{});
  trackEntries.emplace_back("Track 1", songA, browser_entry::PlayTrack{0});
  trackEntries.emplace_back("Track 4", songA, browser_entry::PlayTrack{3});
  trackEntries.emplace_back("Cover", cover, browser_entry::OpenFile{});
  trackEntries.emplace_back("Track 9", songA, browser_entry::PlayTrack{8});
  trackEntries.emplace_back("Folder", "C:/Media/Sub",
                            browser_entry::OpenDirectory{});
  const BrowserEntry audioFileEntry("Song", songB,
                                    browser_entry::OpenFile{});
  const std::optional<PlaybackTarget> audioFileTarget =
      browser_playback_source::targetFor(audioFileEntry);
  ok &= expect(audioFileTarget && isFileTarget(*audioFileTarget, songB),
               "activating a regular audio file must produce a direct "
               "playback target rather than a browser-navigation request");
  const int resolvesBeforeExactTransport = resolveCalls;
  std::optional<playback_queue::Queue::PreparedActivation> exactTrack =
      queue.prepareStart(routeFor(trackTarget(songA, 3)),
                         browser_playback_source::capture(trackEntries));
  ok &= expect(exactTrack.has_value(),
               "an exact track-browser source must prepare");
  queue.commit(std::move(*exactTrack));
  std::optional<playback_queue::Queue::PreparedActivation> previousTrack =
      queue.prepareTransport(playback_queue::Direction::Previous);
  ok &=
      expect(previousTrack &&
                 isTrackTarget(previousTrack->route().target, songA, 0),
             "track-browser previous must preserve exact track order");

  exactTrack = queue.prepareStart(
      routeFor(trackTarget(songA, 3)),
      browser_playback_source::capture(trackEntries));
  queue.commit(std::move(*exactTrack));
  std::optional<playback_queue::Queue::PreparedActivation> nextTrack =
      queue.prepareTransport(playback_queue::Direction::Next);
  ok &= expect(nextTrack &&
                   isTrackTarget(nextTrack->route().target, songA, 8),
               "track-browser next must preserve exact track order");
  ok &= expect(resolveCalls == resolvesBeforeExactTransport,
               "exact track targets must not invoke the path resolver");

  std::optional<playback_queue::Queue::PreparedActivation> singleton =
      queue.prepareStart(routeFor(trackTarget(songC, 2)),
                         playback_queue::singleSource(
                             trackTarget(songC, 2)));
  ok &= expect(singleton.has_value(),
               "a direct action must prepare an explicit singleton source");
  queue.commit(std::move(*singleton));
  ok &= expect(!queue.prepareTransport(playback_queue::Direction::Previous) &&
                   !queue.prepareTransport(playback_queue::Direction::Next),
               "an explicit singleton source must have no neighbours");

#ifdef _WIN32
  std::optional<playback_queue::Queue::PreparedActivation> windowsPath =
      queue.prepareStart(
          routeFor(trackTarget(std::filesystem::path("c:\\media\\a.FLAC"),
                               0)),
          playback_queue::sourceFromFiles(
              {std::filesystem::path("C:/Media/A.flac"), songB}));
  ok &= expect(windowsPath.has_value(),
               "Windows source matching must use ordinal path identity");
  queue.commit(std::move(*windowsPath));
  std::optional<playback_queue::Queue::PreparedActivation> windowsNext =
      queue.prepareTransport(playback_queue::Direction::Next);
  ok &= expect(windowsNext &&
                   isTrackTarget(windowsNext->route().target, songB, 0),
               "Windows transport must preserve path-identity matching");
#endif

  // A file opened from the shell must carry its folder as transport
  // neighbourhood. Without it previous/next stay inert for the whole session
  // while play/pause keeps working, because only transport consults the queue.
  std::error_code fixtureError;
  const std::filesystem::path fixtureRoot =
      std::filesystem::temp_directory_path(fixtureError) /
      "radioify_queue_neighbourhood_tests";
  std::filesystem::remove_all(fixtureRoot, fixtureError);
  std::filesystem::create_directories(fixtureRoot, fixtureError);
  ok &= expect(!fixtureError,
               "the neighbourhood fixture directory must be creatable");
  if (!fixtureError) {
    const std::filesystem::path first = fixtureRoot / "01 first.flac";
    const std::filesystem::path opened = fixtureRoot / "02 opened.flac";
    const std::filesystem::path third = fixtureRoot / "03 third.mp3";
    const std::filesystem::path artwork = fixtureRoot / "cover.jpg";
    for (const std::filesystem::path& file : {first, opened, third, artwork}) {
      std::ofstream(file) << "x";
    }

    const playback_queue::Queue::Services fileServices{
        [](const std::filesystem::path& file)
            -> std::optional<PlaybackTarget> {
          return playbackFileTarget(file);
        },
        [](const PlaybackTarget& target) { return routeFor(target); }};

    playback_queue::Queue shellQueue(fileServices);
    std::optional<playback_queue::Queue::PreparedActivation> shellOpen =
        shellQueue.prepareStart(
            routeFor(playbackFileTarget(opened)),
            playback_queue::sourceFromFileNeighbourhood(opened));
    ok &= expect(shellOpen.has_value(),
                 "opening one file must still prepare a playable queue");
    if (shellOpen) {
      shellQueue.commit(std::move(*shellOpen));
      const std::optional<playback_queue::Queue::PreparedActivation> shellNext =
          shellQueue.prepareTransport(playback_queue::Direction::Next);
      const std::optional<playback_queue::Queue::PreparedActivation>
          shellPrevious =
              shellQueue.prepareTransport(playback_queue::Direction::Previous);
      ok &= expect(shellNext && isFileTarget(shellNext->route().target, third),
                   "an opened file must reach the next track in its folder");
      ok &= expect(
          shellPrevious && isFileTarget(shellPrevious->route().target, first),
          "an opened file must reach the previous track in its folder");
    }

    // Cover art sitting beside the music is not a transport destination.
    playback_queue::Queue artworkQueue(fileServices);
    std::optional<playback_queue::Queue::PreparedActivation> lastTrack =
        artworkQueue.prepareStart(
            routeFor(playbackFileTarget(third)),
            playback_queue::sourceFromFileNeighbourhood(third));
    ok &= expect(lastTrack.has_value(),
                 "the last track in a folder must still prepare a queue");
    if (lastTrack) {
      artworkQueue.commit(std::move(*lastTrack));
      ok &= expect(
          !artworkQueue.prepareTransport(playback_queue::Direction::Next),
          "non-playable siblings must not become transport targets");
    }

    std::filesystem::remove_all(fixtureRoot, fixtureError);
  }

  // A folder that cannot be listed must still leave the opened file playable.
  ok &= expect(
      queue
          .prepareStart(routeFor(trackTarget(songA, 0)),
                        playback_queue::sourceFromFileNeighbourhood(songA))
          .has_value(),
      "an unscannable folder must fall back to the opened file itself");

  return ok ? 0 : 1;
}
