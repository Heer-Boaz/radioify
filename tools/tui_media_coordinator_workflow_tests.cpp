#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
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
    startedFiles.push_back(file);
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
  std::vector<std::filesystem::path> startedFiles;
  std::optional<double> lastSeekRatio;

 private:
  AudioPlaybackSnapshot snapshot_;
};

enum class VideoOpenPlan {
  ReadyNow,
  ReadyOnPump,
  CancelNow,
};

struct VideoSessionRecord {
  std::filesystem::path file;
  VideoOpenPlan openPlan = VideoOpenPlan::ReadyNow;
  bool opening = false;
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
    switch (record_->openPlan) {
      case VideoOpenPlan::ReadyNow:
        record_->ready = true;
        return playback_session::OpenReady{};
      case VideoOpenPlan::ReadyOnPump:
        record_->opening = true;
        return std::nullopt;
      case VideoOpenPlan::CancelNow:
        return playback_session::OpenCancelled{};
    }
    std::abort();
  }
  std::optional<playback_session::OpenOutcome> pumpOpen() override {
    if (!record_->opening) return std::nullopt;
    record_->opening = false;
    record_->ready = true;
    return playback_session::OpenReady{};
  }
  bool opening() const override { return record_->opening; }
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
  bool rejectNextVideoSession = false;
  VideoOpenPlan nextVideoOpenPlan = VideoOpenPlan::ReadyNow;
  playback_session::VideoSessionFactory createSession =
      [&](playback_session::VideoSessionRequest request)
      -> std::unique_ptr<playback_session::VideoSession> {
        if (std::exchange(rejectNextVideoSession, false)) return nullptr;
        auto record = std::make_shared<VideoSessionRecord>();
        record->file = request.file;
        record->openPlan =
            std::exchange(nextVideoOpenPlan, VideoOpenPlan::ReadyNow);
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

  const PlaybackControlSessionId secondVideoControlSession =
      coordinator.controlSessionId();
  const std::filesystem::path firstAudio = "first.flac";
  const std::filesystem::path secondAudio = "second.flac";
  ok &= expect(
      coordinator.startPlayback(
          routeFor(firstAudio),
          playback_queue::sourceFromFiles({firstAudio, secondAudio})) &&
          sessions.back()->handoffResolutions.size() == 1 &&
          sessions.back()->handoffResolutions.front().second,
      "switching from video to audio must use the same accepted handoff "
      "workflow");

  const TuiMediaCoordinator::PollResult audioHandoff = coordinator.poll();
  const PlaybackControlSessionId firstAudioControlSession =
      coordinator.controlSessionId();
  ok &= expect(audioHandoff.playbackChanged && !coordinator.videoReady() &&
                   audio.startedFiles ==
                       std::vector<std::filesystem::path>{firstAudio} &&
                   firstAudioControlSession.valid() &&
                   firstAudioControlSession != secondVideoControlSession,
               "completing the handoff must activate audio and rotate "
               "control ownership");

  ok &= expect(
      !coordinator.handleSystemControlCommand(
          {secondVideoControlSession, PlaybackControlCommand::Pause}) &&
          coordinator.handleSystemControlCommand(
              {firstAudioControlSession, PlaybackControlCommand::Pause}) &&
          coordinator.seekToRatio(0.4) && audio.pauseRequests == 1 &&
          audio.lastSeekRatio == 0.4,
      "audio controls must accept only the active identity and route seek "
      "through the audio session");

  ok &= expect(
      coordinator.handleSystemControlCommand(
          {firstAudioControlSession, PlaybackControlCommand::Next}) &&
          audio.startedFiles == std::vector<std::filesystem::path>{
                                    firstAudio, secondAudio} &&
          coordinator.controlSessionId().valid() &&
          coordinator.controlSessionId() != firstAudioControlSession,
      "queue transport must activate the adjacent item and rotate control "
      "ownership as one workflow");

  const PlaybackControlSessionId secondAudioControlSession =
      coordinator.controlSessionId();
  ok &= expect(
      !coordinator.handleSystemControlCommand(
          {firstAudioControlSession, PlaybackControlCommand::Play}) &&
          coordinator.handleSystemControlCommand(
              {secondAudioControlSession, PlaybackControlCommand::Stop}) &&
          audio.playRequests == 0 && audio.stopRequests == 1 &&
          !coordinator.controlSessionId().valid(),
      "stopping audio must retire its identity without accepting commands "
      "from the previous queue item");

  const std::filesystem::path rejectedVideo = "rejected.mp4";
  rejectNextVideoSession = true;
  ok &= expect(
      coordinator.startPlayback(
          routeFor(rejectedVideo),
          playback_queue::singleSource(playbackFileTarget(rejectedVideo))) &&
          !coordinator.videoReady() &&
          !coordinator.controlSessionId().valid(),
      "a rejected video factory must leave no partial session or control "
      "identity");
  const TuiMediaCoordinator::PollResult rejectedVideoResult =
      coordinator.poll();
  const bool reportedRejectedVideo = std::any_of(
      rejectedVideoResult.events.begin(), rejectedVideoResult.events.end(),
      [&](const TuiMediaCoordinator::Event& event) {
        const auto* failure =
            std::get_if<TuiMediaCoordinator::VideoPlaybackFailed>(&event);
        return failure && failure->file == rejectedVideo;
      });
  ok &= expect(reportedRejectedVideo,
               "a rejected factory must publish a typed playback failure");

  const std::filesystem::path cancelledVideo = "cancelled.mp4";
  const std::filesystem::path uncommittedSuccessor = "not-activated.mp4";
  nextVideoOpenPlan = VideoOpenPlan::CancelNow;
  ok &= expect(
      coordinator.startPlayback(
          routeFor(cancelledVideo),
          playback_queue::sourceFromFiles(
              {cancelledVideo, uncommittedSuccessor})) &&
          !coordinator.videoReady() &&
          !coordinator.controlSessionId().valid() &&
          !queue.prepareTransport(playback_queue::Direction::Next),
      "a cancelled video open must discard the session and its prepared "
      "queue activation");

  const std::filesystem::path thirdVideo = "third.mp4";
  nextVideoOpenPlan = VideoOpenPlan::ReadyOnPump;
  ok &= expect(
      coordinator.startPlayback(routeFor(thirdVideo),
                                playback_queue::singleSource(
                                    playbackFileTarget(thirdVideo))) &&
          !coordinator.videoReady() && sessions.size() == 4 &&
          !coordinator.controlSessionId().valid(),
      "an asynchronous video open must retain activation without publishing "
      "control ownership early");

  const TuiMediaCoordinator::PollResult thirdVideoOpen = coordinator.poll();
  ok &= expect(thirdVideoOpen.playbackChanged && coordinator.videoReady() &&
                   coordinator.controlSessionId().valid(),
               "a completed asynchronous open must atomically activate the "
               "video and its control identity");

  const PlaybackControlSessionId thirdVideoControlSession =
      coordinator.controlSessionId();
  ok &= expect(
      coordinator.handleSystemControlCommand(
          {thirdVideoControlSession, PlaybackControlCommand::Stop}) &&
          sessions.back()->controlCommands ==
              std::vector<PlaybackControlCommand>{
                  PlaybackControlCommand::Stop} &&
          !coordinator.controlSessionId().valid(),
      "video stop must reach the active endpoint before retiring its control "
      "identity");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
