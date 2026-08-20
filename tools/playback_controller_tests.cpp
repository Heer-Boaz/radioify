#include <functional>
#include <iostream>
#include <memory>
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

bool activated(playback_controller::ActivationOutcome outcome) {
  return outcome == playback_controller::ActivationOutcome::Activated;
}

bool handled(playback_controller::ActivationOutcome outcome) {
  return outcome == playback_controller::ActivationOutcome::Handled;
}

bool rejected(playback_controller::ActivationOutcome outcome) {
  return outcome == playback_controller::ActivationOutcome::Rejected;
}

class CallbackSession final : public playback_controller::ActivePresentation {
 public:
  explicit CallbackSession(std::function<void()> run) : run_(std::move(run)) {}

  void run() override { run_(); }

 private:
  std::function<void()> run_;
};

playback_controller::PresentationOpenResult activate(
    std::function<void()> run = {}) {
  std::unique_ptr<playback_controller::ActivePresentation> session;
  if (run) {
    session = std::make_unique<CallbackSession>(std::move(run));
    return playback_controller::PresentationSession{std::move(session)};
  }
  return playback_controller::PresentationStarted{};
}

class LambdaPresenter final : public playback_controller::Presenter {
 public:
  using Open = std::function<playback_controller::PresentationOpenResult(
      const playback_route::Route&, playback_controller::SessionCommands&)>;

  explicit LambdaPresenter(Open open) : open_(std::move(open)) {}

  playback_controller::PresentationOpenResult open(
      const playback_route::Route& route,
      playback_controller::SessionCommands& commands) override {
    return open_(route, commands);
  }

 private:
  Open open_;
};

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
  LambdaPresenter accept([&](const playback_route::Route& route,
                             playback_controller::SessionCommands&) {
    presented.push_back(route.target);
    return activate();
  });

  std::vector<std::filesystem::path> requestedFiles{songA, skipped, songB,
                                                    songC};
  playback_controller::Source files =
      playback_controller::sourceFromFiles(requestedFiles);
  requestedFiles.clear();
  ok &= expect(activated(controller.start(routeFor({songB, 0}),
                                          std::move(files), accept)),
               "a source containing its target must start");
  ok &= expect(resolveCalls == 0,
               "starting playback must not eagerly resolve source items");
  ok &= expect(presented.size() == 1 && isTarget(presented.back(), songB, 0),
               "the controller must present the requested source item");

  presented.clear();
  ok &=
      expect(activated(controller.transport(
                 playback_controller::Direction::Previous, accept)) &&
                 presented.size() == 1 && isTarget(presented.back(), songA, 0),
             "previous must lazily skip an unresolvable source item");
  ok &= expect(resolveCalls > 0,
               "transport must resolve path-only items at the playback owner");

  ok &= expect(activated(controller.transport(
                   playback_controller::Direction::Next, accept)) &&
                   isTarget(presented.back(), songB, 0),
               "transport must advance from the committed source position");

  const std::size_t presentationsBeforeInvalidStart = presented.size();
  ok &= expect(rejected(controller.start(
                   routeFor({songB, 0}),
                   playback_controller::sourceFromFiles({unrelated}), accept)),
               "a source missing its requested target must be rejected");
  ok &= expect(presented.size() == presentationsBeforeInvalidStart,
               "an invalid source must never reach the presenter");
  ok &= expect(activated(controller.transport(
                   playback_controller::Direction::Next, accept)) &&
                   isTarget(presented.back(), songC, 0),
               "an invalid source must leave the active source intact");

  ok &= expect(activated(controller.start(routeFor({songB, 0}),
                                          playback_controller::sourceFromFiles(
                                              {songA, skipped, songB, songC}),
                                          accept)),
               "the original source must be restorable");
  PlaybackTarget handledTarget;
  LambdaPresenter handleWithoutPlayback(
      [&](const playback_route::Route& route,
          playback_controller::SessionCommands&) {
        handledTarget = route.target;
        return playback_controller::PresentationOpenResult(
            playback_controller::PresentationHandled{});
      });
  ok &= expect(handled(controller.start(
                   routeFor({unrelated, 0}),
                   playback_controller::singleSource({unrelated, 0}),
                   handleWithoutPlayback)) &&
                   isTarget(handledTarget, unrelated, 0),
               "a handled presentation must not activate playback");
  PlaybackTarget failedTarget;
  LambdaPresenter reject([&](const playback_route::Route& route,
                             playback_controller::SessionCommands&) {
    failedTarget = route.target;
    return playback_controller::PresentationOpenResult(
        playback_controller::PresentationRejected{});
  });
  ok &=
      expect(rejected(controller.start(
                 routeFor({unrelated, 0}),
                 playback_controller::singleSource({unrelated, 0}), reject)) &&
                 isTarget(failedTarget, unrelated, 0),
             "a backend open failure must reject a replacement source");
  ok &= expect(rejected(controller.transport(
                   playback_controller::Direction::Next, reject)) &&
                   isTarget(failedTarget, songC, 0),
               "a failed replacement must preserve the committed position");
  presented.clear();
  ok &=
      expect(activated(controller.transport(
                 playback_controller::Direction::Next, accept)) &&
                 presented.size() == 1 && isTarget(presented.back(), songC, 0),
             "a failed open must not commit its candidate position");

  bool openPhaseRequestAccepted = true;
  LambdaPresenter requestDuringOpen(
      [&](const playback_route::Route&,
          playback_controller::SessionCommands& commands) {
        openPhaseRequestAccepted =
            commands.transport(playback_controller::Direction::Next);
        return activate();
      });
  ok &= expect(activated(controller.start(routeFor({songB, 0}),
                                          playback_controller::sourceFromFiles(
                                              {songA, skipped, songB, songC}),
                                          requestDuringOpen)),
               "a valid backend open must activate");
  ok &= expect(!openPhaseRequestAccepted,
               "successor commands must stay closed during backend open");
  presented.clear();
  ok &=
      expect(activated(controller.transport(
                 playback_controller::Direction::Next, accept)) &&
                 presented.size() == 1 && isTarget(presented.back(), songC, 0),
             "an open-phase command must not alter playback state");

  std::vector<PlaybackTarget> successorTargets;
  bool firstRunRequestAccepted = false;
  bool duplicateRunRequestAccepted = true;
  LambdaPresenter transportNextDuringRun(
      [&](const playback_route::Route& route,
          playback_controller::SessionCommands& commands) {
        successorTargets.push_back(route.target);
        if (isTarget(route.target, songB, 0)) {
          return activate([&]() {
            firstRunRequestAccepted =
                commands.transport(playback_controller::Direction::Next);
            duplicateRunRequestAccepted =
                commands.transport(playback_controller::Direction::Next);
          });
        }
        return activate();
      });
  ok &= expect(activated(controller.start(routeFor({songB, 0}),
                                          playback_controller::sourceFromFiles(
                                              {songA, skipped, songB, songC}),
                                          transportNextDuringRun)),
               "an active session may hand off to one successor");
  ok &= expect(firstRunRequestAccepted && !duplicateRunRequestAccepted,
               "an active presentation must accept exactly one successor");
  ok &= expect(successorTargets.size() == 2 &&
                   isTarget(successorTargets[0], songB, 0) &&
                   isTarget(successorTargets[1], songC, 0),
               "the controller must drive the complete successor lifecycle");

  std::vector<PlaybackTarget> failedSuccessorTargets;
  LambdaPresenter rejectSuccessor(
      [&](const playback_route::Route& route,
          playback_controller::SessionCommands& commands) {
        failedSuccessorTargets.push_back(route.target);
        if (isTarget(route.target, songB, 0)) {
          return activate([&]() {
            (void)commands.transport(playback_controller::Direction::Next);
          });
        }
        return playback_controller::PresentationOpenResult(
            playback_controller::PresentationRejected{});
      });
  ok &= expect(rejected(controller.start(routeFor({songB, 0}),
                                         playback_controller::sourceFromFiles(
                                             {songA, skipped, songB, songC}),
                                         rejectSuccessor)),
               "a rejected successor must fail the chained activation");
  ok &= expect(failedSuccessorTargets.size() == 2 &&
                   isTarget(failedSuccessorTargets[0], songB, 0) &&
                   isTarget(failedSuccessorTargets[1], songC, 0),
               "a chained successor must reach backend open");
  presented.clear();
  ok &=
      expect(activated(controller.transport(
                 playback_controller::Direction::Previous, accept)) &&
                 presented.size() == 1 && isTarget(presented.back(), songA, 0),
             "a failed successor must preserve the last opened activation");

  LambdaPresenter throwDuringOpen(
      [](const playback_route::Route&, playback_controller::SessionCommands&)
          -> playback_controller::PresentationOpenResult {
        throw std::runtime_error("open failed");
      });
  bool openExceptionObserved = false;
  try {
    (void)controller.start(routeFor({unrelated, 0}),
                           playback_controller::singleSource({unrelated, 0}),
                           throwDuringOpen);
  } catch (const std::runtime_error&) {
    openExceptionObserved = true;
  }
  ok &= expect(openExceptionObserved,
               "a backend open exception must propagate to its owner");
  presented.clear();
  ok &=
      expect(activated(controller.transport(
                 playback_controller::Direction::Next, accept)) &&
                 presented.size() == 1 && isTarget(presented.back(), songB, 0),
             "an open exception must leave committed state unchanged");

  LambdaPresenter throwDuringRun(
      [](const playback_route::Route&, playback_controller::SessionCommands&) {
        return activate([]() { throw std::runtime_error("run failed"); });
      });
  bool runExceptionObserved = false;
  try {
    (void)controller.start(
        routeFor({songB, 0}),
        playback_controller::sourceFromFiles({songA, skipped, songB, songC}),
        throwDuringRun);
  } catch (const std::runtime_error&) {
    runExceptionObserved = true;
  }
  ok &= expect(runExceptionObserved,
               "an active-session exception must propagate to its owner");
  presented.clear();
  ok &=
      expect(activated(controller.transport(
                 playback_controller::Direction::Next, accept)) &&
                 presented.size() == 1 && isTarget(presented.back(), songC, 0),
             "an opened presentation must commit before its modal run");

  std::vector<BrowserEntry> trackEntries;
  trackEntries.emplace_back("Info", songA, browser_entry::Information{});
  trackEntries.emplace_back("Track 1", songA, browser_entry::PlayTrack{0});
  trackEntries.emplace_back("Track 4", songA, browser_entry::PlayTrack{3});
  trackEntries.emplace_back("Cover", cover, browser_entry::OpenFile{});
  trackEntries.emplace_back("Track 9", songA, browser_entry::PlayTrack{8});
  trackEntries.emplace_back("Folder", "C:/Media/Sub",
                            browser_entry::OpenDirectory{});
  const int resolvesBeforeExactTransport = resolveCalls;
  ok &= expect(activated(controller.start(
                   routeFor({songA, 3}),
                   browser_playback_source::capture(trackEntries), accept)),
               "an exact track-browser source must start");
  presented.clear();
  ok &= expect(activated(controller.transport(
                   playback_controller::Direction::Previous, accept)) &&
                   isTarget(presented.back(), songA, 0),
               "track-browser previous must preserve exact track order");
  ok &= expect(activated(controller.start(
                   routeFor({songA, 3}),
                   browser_playback_source::capture(trackEntries), accept)) &&
                   activated(controller.transport(
                       playback_controller::Direction::Next, accept)) &&
                   isTarget(presented.back(), songA, 8),
               "track-browser next must preserve exact track order");
  ok &= expect(resolveCalls == resolvesBeforeExactTransport,
               "exact track targets must not invoke the path resolver");

  ok &= expect(activated(controller.start(
                   routeFor({songC, 2}),
                   playback_controller::singleSource({songC, 2}), accept)),
               "a direct action must request an explicit singleton source");
  ok &= expect(rejected(controller.transport(
                   playback_controller::Direction::Previous, accept)) &&
                   rejected(controller.transport(
                       playback_controller::Direction::Next, accept)),
               "an explicit singleton source must have no neighbours");

#ifdef _WIN32
  ok &= expect(activated(controller.start(
                   routeFor({std::filesystem::path("c:\\media\\a.FLAC"), 0}),
                   playback_controller::sourceFromFiles(
                       {std::filesystem::path("C:/Media/A.flac"), songB}),
                   accept)),
               "Windows source matching must use ordinal path identity");
  presented.clear();
  ok &=
      expect(activated(controller.transport(
                 playback_controller::Direction::Next, accept)) &&
                 presented.size() == 1 && isTarget(presented.back(), songB, 0),
             "Windows transport must preserve path-identity matching");
#endif

  return ok ? 0 : 1;
}
