#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "app/media_processing_coordinator.h"
#include "app/playback_queue.h"
#include "audio/playback_session.h"
#include "playback/session/video_session.h"
#include "playback/target.h"
#include "tui/media_coordinator.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "tui_media_coordinator_workflow_tests: " << message << '\n';
  return false;
}

class RecordingAudioSession final : public audio_playback::Session {
 public:
  bool startFile(const std::filesystem::path& file, int trackIndex) override {
    snapshot_.source = AudioPlaybackSource{file, trackIndex};
    snapshot_.ready = true;
    return true;
  }

  void stop() override {
    ++stopRequests;
    snapshot_ = {};
  }
  AudioPlaybackSnapshot snapshot() const override { return snapshot_; }
  void play() override { ++playRequests; }
  void pause() override { ++pauseRequests; }
  void togglePause() override { ++toggleRequests; }
  void seekToRatio(double ratio) override { lastSeekRatio = ratio; }

  int playRequests = 0;
  int pauseRequests = 0;
  int toggleRequests = 0;
  int stopRequests = 0;
  std::optional<double> lastSeekRatio;

 private:
  AudioPlaybackSnapshot snapshot_;
};

struct VideoSessionRecord {
  std::filesystem::path file;
  bool ready = false;
  int windowToggleRequests = 0;
  std::vector<PlaybackControlCommand> controlCommands;
  std::vector<std::pair<playback_session_exit::RequestId, bool>>
      handoffResolutions;
  bool completeOnNextPump = false;
};

class RecordingVideoSession final : public playback_session::VideoSession {
 public:
  explicit RecordingVideoSession(std::shared_ptr<VideoSessionRecord> record)
      : record_(std::move(record)) {}

  std::optional<playback_session::OpenOutcome> startOpen() override {
    record_->ready = true;
    return playback_session::OpenReady{};
  }
  std::optional<playback_session::OpenOutcome> pumpOpen() override {
    return std::nullopt;
  }
  bool opening() const override { return false; }
  bool ready() const override { return record_->ready; }
  std::optional<playback_session::TransitionSnapshot> transitionSnapshot()
      const override {
    return std::nullopt;
  }
  std::optional<PlaybackSessionCompletion> pump() override {
    if (!record_->completeOnNextPump) return std::nullopt;
    record_->completeOnNextPump = false;
    PlaybackSessionCompletion completion;
    completion.continuityState.presentation = presentation_;
    return completion;
  }
  PlaybackShellTerminalRole terminalRole() const override {
    return presentation_.terminalRole();
  }
  std::vector<NativeWaitHandle> activityWaitHandles() const override {
    return {};
  }
  wake_schedule::Deadline nextWakeDeadline() const override {
    return std::nullopt;
  }
  PlaybackControlState controlState() const override {
    PlaybackControlState state(playbackFileTarget(record_->file), true);
    state.status = PlaybackControlStatus::Playing;
    return state;
  }
  PlaybackPresentationState presentationState() const override {
    return presentation_;
  }
  bool capturesBrowserInput() const override { return false; }
  void setExternalInputModal(bool) override {}
  bool handleInputEvent(const InputEvent&) override { return false; }
  bool pollWindowInput(InputEvent&) override { return false; }
  bool handleWindowInputEvent(const InputEvent&) override { return false; }
  bool handleControlCommand(PlaybackControlCommand command) override {
    record_->controlCommands.push_back(command);
    return true;
  }
  bool seekToRatio(double) override { return true; }
  bool toggleWindowPresentation() override {
    ++record_->windowToggleRequests;
    presentation_ = presentation_.toggleWindowMode();
    return true;
  }
  bool togglePictureInPicture() override { return false; }
  bool toggleFullscreen() override { return false; }
  bool activatePresentation() override { return true; }
  std::optional<playback_session_exit::RequestId> requestHandoff() override {
    const playback_session_exit::RequestId requestId = nextRequestId_++;
    playback_session_exit::HandoffRequest request;
    request.id = requestId;
    request.intent = playback_session_exit::ExternalHandoff{};
    events_.emplace_back(std::move(request));
    return requestId;
  }
  bool resolveHandoff(playback_session_exit::RequestId requestId,
                      bool accepted) override {
    record_->handoffResolutions.emplace_back(requestId, accepted);
    if (accepted) record_->completeOnNextPump = true;
    return true;
  }
  std::vector<playback_session::Event> drainEvents() override {
    std::vector<playback_session::Event> result;
    result.swap(events_);
    return result;
  }
  void mediaTaskFinished(
      const playback_media_processing::Completion&) override {}
  void mediaTaskActivityChanged(
      std::optional<playback_media_processing::Activity>) override {}
  void requestStop() override { record_->completeOnNextPump = true; }
  void requestQuit() override { record_->completeOnNextPump = true; }

 private:
  std::shared_ptr<VideoSessionRecord> record_;
  PlaybackPresentationState presentation_ =
      PlaybackPresentationState::terminalAscii();
  playback_session_exit::RequestId nextRequestId_ = 1;
  std::vector<playback_session::Event> events_;
};

playback_route::Route routeFor(const std::filesystem::path& file) {
  playback_route::Route route;
  route.target = playbackFileTarget(file);
  return route;
}

}  // namespace

int main() {
  bool ok = true;
  RecordingAudioSession audio;
  media_processing::Coordinator processing(
      media_processing::Coordinator::Operations{});
  playback_media_processing::Actions processingActions(processing);

  playback_queue::Queue queue(
      {[](const std::filesystem::path& file) {
         return std::optional<PlaybackTarget>(playbackFileTarget(file));
       },
       [](const PlaybackTarget& target) {
         playback_route::Route route;
         route.target = target;
         return route;
       }});

  std::vector<std::shared_ptr<VideoSessionRecord>> sessions;
  playback_session::VideoSessionFactory createSession =
      [&](playback_session::VideoSessionRequest request) {
        auto record = std::make_shared<VideoSessionRecord>();
        record->file = request.file;
        sessions.push_back(record);
        return std::make_unique<RecordingVideoSession>(std::move(record));
      };

  VideoPlaybackConfig config;
  TuiMediaCoordinator coordinator(
      {queue, processing, processingActions, audio,
       std::move(createSession), config});

  const std::filesystem::path first = "first.mp4";
  const std::filesystem::path second = "second.mp4";
  ok &= expect(
      coordinator.startPlayback(
          routeFor(first),
          playback_queue::sourceFromFiles({first, second})) &&
          coordinator.videoReady() && sessions.size() == 1 &&
          sessions.front()->file == first,
      "opening a video must create and activate exactly one video session");

  const PlaybackControlSessionId firstControlSession =
      coordinator.controlSessionId();
  ok &= expect(firstControlSession.valid(),
               "accepted playback must own a control-session identity");

  ok &= expect(coordinator.toggleWindowPresentation() &&
                   sessions.front()->windowToggleRequests == 1 &&
                   sessions.front()->controlCommands.empty() &&
                   audio.pauseRequests == 0 && audio.toggleRequests == 0,
               "switching presentation must not synthesize audio or video "
               "transport commands");

  const PlaybackControlCommandEvent stalePause{
      PlaybackControlSessionId{firstControlSession.value + 1},
      PlaybackControlCommand::Pause};
  const PlaybackControlCommandEvent currentPause{
      firstControlSession, PlaybackControlCommand::Pause};
  ok &= expect(!coordinator.handleSystemControlCommand(stalePause) &&
                   coordinator.handleSystemControlCommand(currentPause) &&
                   sessions.front()->controlCommands ==
                       std::vector<PlaybackControlCommand>{
                           PlaybackControlCommand::Pause},
               "system transport must reject stale ownership and dispatch "
               "one current-session command");

  ok &= expect(coordinator.startFiles(routeFor(second), {first, second}) &&
                   sessions.size() == 1 &&
                   sessions.front()->handoffResolutions.size() == 1 &&
                   sessions.front()->handoffResolutions.front().second,
               "changing media must resolve an identity-bound handoff before "
               "replacing the active session");

  const TuiMediaCoordinator::PollResult handoff = coordinator.poll();
  ok &= expect(handoff.playbackChanged && sessions.size() == 2 &&
                   sessions.back()->file == second &&
                   coordinator.videoReady() &&
                   coordinator.controlSessionId().valid() &&
                   coordinator.controlSessionId() != firstControlSession,
               "pumping an accepted handoff must close the old session, "
               "activate the requested video and rotate control identity");

  ok &= expect(!coordinator.handleSystemControlCommand(currentPause) &&
                   sessions.back()->controlCommands.empty(),
               "commands from the retired session must not reach its "
               "replacement");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
