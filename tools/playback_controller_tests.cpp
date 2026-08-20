#include <iostream>
#include <stdexcept>
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

  std::vector<PlaybackTarget> presented;
  const playback_controller::Controller::Presenter accept =
      [&](const playback_route::Route& route, playback_controller::Handoff&) {
        presented.push_back(route.target);
        return true;
      };

  std::vector<std::filesystem::path> requestedFiles{songA, skipped, songB,
                                                    songC};
  playback_controller::Source files =
      playback_controller::sourceFromFiles(requestedFiles);
  requestedFiles.clear();
  ok &= expect(controller.start(routeFor({songB, 0}), std::move(files), accept),
               "a source containing its target must start");
  ok &= expect(resolveCalls == 0,
               "starting playback must not eagerly resolve source items");
  ok &= expect(presented.size() == 1 && isTarget(presented.back(), songB, 0),
               "the controller must present the requested source item");

  presented.clear();
  ok &= expect(
      controller.transport(playback_controller::Direction::Previous, accept) &&
          presented.size() == 1 && isTarget(presented.back(), songA, 0),
      "previous must lazily skip an unresolvable source item");
  ok &= expect(resolveCalls > 0,
               "transport must resolve path-only items at the playback owner");

  ok &= expect(
      controller.transport(playback_controller::Direction::Next, accept) &&
          isTarget(presented.back(), songB, 0),
      "transport must advance from the controller-owned current position");

  const std::size_t presentationsBeforeInvalidStart = presented.size();
  ok &= expect(!controller.start(
                   routeFor({songB, 0}),
                   playback_controller::sourceFromFiles({unrelated}), accept),
               "a source missing its requested target must be rejected");
  ok &= expect(presented.size() == presentationsBeforeInvalidStart,
               "an invalid source must never reach the presenter");
  ok &= expect(
      controller.transport(playback_controller::Direction::Next, accept) &&
          isTarget(presented.back(), songC, 0),
      "a rejected source must leave the active source intact");

  ok &= expect(controller.start(routeFor({songB, 0}),
                                playback_controller::sourceFromFiles(
                                    {songA, skipped, songB, songC}),
                                accept),
               "the original source must be restorable");
  PlaybackTarget failedTarget;
  const playback_controller::Controller::Presenter reject =
      [&](const playback_route::Route& route, playback_controller::Handoff&) {
        failedTarget = route.target;
        return false;
      };
  ok &= expect(!controller.start(
                   routeFor({unrelated, 0}),
                   playback_controller::singleSource({unrelated, 0}), reject) &&
                   isTarget(failedTarget, unrelated, 0),
               "a presenter failure must reject a valid replacement source");
  ok &= expect(
      !controller.transport(playback_controller::Direction::Next, reject) &&
          isTarget(failedTarget, songC, 0),
      "a failed replacement must preserve the previous source and position");
  presented.clear();
  ok &= expect(
      controller.transport(playback_controller::Direction::Next, accept) &&
          presented.size() == 1 && isTarget(presented.back(), songC, 0),
      "a failed activation must not commit its candidate position");

  std::vector<PlaybackTarget> handoffTargets;
  const playback_controller::Controller::Presenter handoffNext =
      [&](const playback_route::Route& route,
          playback_controller::Handoff& handoff) {
        handoffTargets.push_back(route.target);
        if (isTarget(route.target, songB, 0)) {
          return handoff.transport(playback_controller::Direction::Next);
        }
        return true;
      };
  ok &= expect(controller.start(routeFor({songB, 0}),
                                playback_controller::sourceFromFiles(
                                    {songA, skipped, songB, songC}),
                                handoffNext),
               "an accepted transport handoff must complete");
  ok &= expect(handoffTargets.size() == 2 &&
                   isTarget(handoffTargets[0], songB, 0) &&
                   isTarget(handoffTargets[1], songC, 0),
               "the controller must drive the complete handoff lifecycle");
  bool invalidHandoffAccepted = true;
  const playback_controller::Controller::Presenter invalidHandoff =
      [&](const playback_route::Route&, playback_controller::Handoff& handoff) {
        invalidHandoffAccepted =
            handoff.start(routeFor({songB, 0}),
                          playback_controller::sourceFromFiles({unrelated}));
        return true;
      };
  ok &= expect(controller.start(routeFor({songB, 0}),
                                playback_controller::sourceFromFiles(
                                    {songA, skipped, songB, songC}),
                                invalidHandoff),
               "rejecting a handoff must not fail the active presentation");
  ok &= expect(!invalidHandoffAccepted,
               "an invalid handoff must be rejected before session exit");
  presented.clear();
  ok &= expect(
      controller.transport(playback_controller::Direction::Next, accept) &&
          presented.size() == 1 && isTarget(presented.back(), songC, 0),
      "an invalid handoff must leave the active source and position intact");

  std::vector<PlaybackTarget> failedHandoffTargets;
  const playback_controller::Controller::Presenter rejectHandoffTarget =
      [&](const playback_route::Route& route,
          playback_controller::Handoff& handoff) {
        failedHandoffTargets.push_back(route.target);
        if (isTarget(route.target, songB, 0)) {
          return handoff.transport(playback_controller::Direction::Next);
        }
        return false;
      };
  ok &= expect(!controller.start(routeFor({songB, 0}),
                                 playback_controller::sourceFromFiles(
                                     {songA, skipped, songB, songC}),
                                 rejectHandoffTarget),
               "a rejected handoff target must fail the chained activation");
  ok &= expect(failedHandoffTargets.size() == 2 &&
                   isTarget(failedHandoffTargets[0], songB, 0) &&
                   isTarget(failedHandoffTargets[1], songC, 0),
               "a chained handoff must present its prepared target");
  presented.clear();
  ok &= expect(
      controller.transport(playback_controller::Direction::Previous, accept) &&
          presented.size() == 1 && isTarget(presented.back(), songA, 0),
      "a failed chained target must preserve the last successful activation");

  const playback_controller::Controller::Presenter throwPresentation =
      [](const playback_route::Route&, playback_controller::Handoff&) -> bool {
    throw std::runtime_error("presentation failed");
  };
  bool presentationExceptionObserved = false;
  try {
    (void)controller.start(routeFor({unrelated, 0}),
                           playback_controller::singleSource({unrelated, 0}),
                           throwPresentation);
  } catch (const std::runtime_error&) {
    presentationExceptionObserved = true;
  }
  ok &= expect(presentationExceptionObserved,
               "a presenter exception must propagate to its owner");
  presented.clear();
  ok &= expect(
      controller.transport(playback_controller::Direction::Next, accept) &&
          presented.size() == 1 && isTarget(presented.back(), songB, 0),
      "a presenter exception must leave the active state unmodified");

  std::vector<BrowserEntry> trackEntries;
  trackEntries.emplace_back("Info", songA, browser_entry::Information{});
  trackEntries.emplace_back("Track 1", songA, browser_entry::PlayTrack{0});
  trackEntries.emplace_back("Track 4", songA, browser_entry::PlayTrack{3});
  trackEntries.emplace_back("Cover", cover, browser_entry::OpenFile{});
  trackEntries.emplace_back("Track 9", songA, browser_entry::PlayTrack{8});
  trackEntries.emplace_back("Folder", "C:/Media/Sub",
                            browser_entry::OpenDirectory{});
  const int resolvesBeforeExactTransport = resolveCalls;
  ok &= expect(
      controller.start(routeFor({songA, 3}),
                       browser_playback_source::capture(trackEntries), accept),
      "an exact track-browser source must start");
  presented.clear();
  ok &= expect(
      controller.transport(playback_controller::Direction::Previous, accept) &&
          isTarget(presented.back(), songA, 0),
      "track-browser previous must preserve exact track order");
  ok &= expect(
      controller.start(routeFor({songA, 3}),
                       browser_playback_source::capture(trackEntries),
                       accept) &&
          controller.transport(playback_controller::Direction::Next, accept) &&
          isTarget(presented.back(), songA, 8),
      "track-browser next must preserve exact track order");
  ok &= expect(resolveCalls == resolvesBeforeExactTransport,
               "exact track targets must not invoke the path resolver");

  ok &= expect(
      controller.start(routeFor({songC, 2}),
                       playback_controller::singleSource({songC, 2}), accept),
      "a direct action must request an explicit singleton source");
  ok &= expect(
      !controller.transport(playback_controller::Direction::Previous, accept) &&
          !controller.transport(playback_controller::Direction::Next, accept),
      "an explicit singleton source must have no neighbours");

#ifdef _WIN32
  ok &= expect(controller.start(
                   routeFor({std::filesystem::path("c:\\media\\a.FLAC"), 0}),
                   playback_controller::sourceFromFiles(
                       {std::filesystem::path("C:/Media/A.flac"), songB}),
                   accept),
               "Windows source matching must use ordinal path identity");
  presented.clear();
  ok &= expect(
      controller.transport(playback_controller::Direction::Next, accept) &&
          presented.size() == 1 && isTarget(presented.back(), songB, 0),
      "Windows transport must preserve path-identity matching");
#endif

  return ok ? 0 : 1;
}
