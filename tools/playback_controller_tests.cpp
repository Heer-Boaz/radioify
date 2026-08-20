#include <iostream>
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
  playback_controller::Controller controller(
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
  playback_controller::Source files =
      playback_controller::sourceFromFiles(requestedFiles);
  requestedFiles.clear();
  std::optional<playback_controller::Controller::PreparedActivation> initial =
      controller.prepareStart(routeFor({songB, 0}), std::move(files));
  ok &= expect(initial && isTarget(initial->route().target, songB, 0),
               "a source containing its target must prepare");
  ok &= expect(resolveCalls == 0,
               "preparing a source must not eagerly resolve its items");
  ok &= expect(
      !controller.prepareTransport(playback_controller::Direction::Previous),
      "transport must remain unavailable before activation commits");
  controller.commit(std::move(*initial));

  std::optional<playback_controller::Controller::PreparedActivation> previous =
      controller.prepareTransport(playback_controller::Direction::Previous);
  ok &= expect(previous && isTarget(previous->route().target, songA, 0),
               "previous must lazily skip an unresolvable source item");
  ok &= expect(resolveCalls > 0,
               "transport must resolve path-only items at the playback owner");

  std::optional<playback_controller::Controller::PreparedActivation>
      nextBeforeCommit =
          controller.prepareTransport(playback_controller::Direction::Next);
  ok &= expect(
      nextBeforeCommit && isTarget(nextBeforeCommit->route().target, songC, 0),
      "preparation must not mutate the committed position");

  controller.commit(std::move(*previous));
  std::optional<playback_controller::Controller::PreparedActivation> next =
      controller.prepareTransport(playback_controller::Direction::Next);
  ok &= expect(next && isTarget(next->route().target, songB, 0),
               "transport must advance from the committed position");
  controller.commit(std::move(*next));

  ok &= expect(!controller.prepareStart(
                   routeFor({songB, 0}),
                   playback_controller::sourceFromFiles({unrelated})),
               "a source missing its requested target must be rejected");
  std::optional<playback_controller::Controller::PreparedActivation>
      nextAfterRejectedSource =
          controller.prepareTransport(playback_controller::Direction::Next);
  ok &= expect(nextAfterRejectedSource &&
                   isTarget(nextAfterRejectedSource->route().target, songC, 0),
               "a rejected source must leave the active source intact");

  std::optional<playback_controller::Controller::PreparedActivation>
      discardedReplacement = controller.prepareStart(
          routeFor({unrelated, 0}),
          playback_controller::singleSource({unrelated, 0}));
  ok &= expect(discardedReplacement &&
                   isTarget(discardedReplacement->route().target, unrelated, 0),
               "a valid replacement source must prepare independently");
  discardedReplacement.reset();
  std::optional<playback_controller::Controller::PreparedActivation>
      nextAfterDiscard =
          controller.prepareTransport(playback_controller::Direction::Next);
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
  std::optional<playback_controller::Controller::PreparedActivation>
      exactTrack = controller.prepareStart(
          routeFor({songA, 3}), browser_playback_source::capture(trackEntries));
  ok &= expect(exactTrack.has_value(),
               "an exact track-browser source must prepare");
  controller.commit(std::move(*exactTrack));
  std::optional<playback_controller::Controller::PreparedActivation>
      previousTrack =
          controller.prepareTransport(playback_controller::Direction::Previous);
  ok &=
      expect(previousTrack && isTarget(previousTrack->route().target, songA, 0),
             "track-browser previous must preserve exact track order");

  exactTrack = controller.prepareStart(
      routeFor({songA, 3}), browser_playback_source::capture(trackEntries));
  controller.commit(std::move(*exactTrack));
  std::optional<playback_controller::Controller::PreparedActivation> nextTrack =
      controller.prepareTransport(playback_controller::Direction::Next);
  ok &= expect(nextTrack && isTarget(nextTrack->route().target, songA, 8),
               "track-browser next must preserve exact track order");
  ok &= expect(resolveCalls == resolvesBeforeExactTransport,
               "exact track targets must not invoke the path resolver");

  std::optional<playback_controller::Controller::PreparedActivation> singleton =
      controller.prepareStart(routeFor({songC, 2}),
                              playback_controller::singleSource({songC, 2}));
  ok &= expect(singleton.has_value(),
               "a direct action must prepare an explicit singleton source");
  controller.commit(std::move(*singleton));
  ok &= expect(
      !controller.prepareTransport(playback_controller::Direction::Previous) &&
          !controller.prepareTransport(playback_controller::Direction::Next),
      "an explicit singleton source must have no neighbours");

#ifdef _WIN32
  std::optional<playback_controller::Controller::PreparedActivation>
      windowsPath = controller.prepareStart(
          routeFor({std::filesystem::path("c:\\media\\a.FLAC"), 0}),
          playback_controller::sourceFromFiles(
              {std::filesystem::path("C:/Media/A.flac"), songB}));
  ok &= expect(windowsPath.has_value(),
               "Windows source matching must use ordinal path identity");
  controller.commit(std::move(*windowsPath));
  std::optional<playback_controller::Controller::PreparedActivation>
      windowsNext =
          controller.prepareTransport(playback_controller::Direction::Next);
  ok &= expect(windowsNext && isTarget(windowsNext->route().target, songB, 0),
               "Windows transport must preserve path-identity matching");
#endif

  return ok ? 0 : 1;
}
