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
#include "app/playback_activation_controller.h"
#include "app/playback_control_router.h"
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
  audio_playback::FileStartResult startFile(
      const std::filesystem::path& file, int trackIndex) override {
    attemptedFiles.push_back(file);
    const audio_playback::FileStartResult result = std::exchange(
        nextStartResult, audio_playback::FileStartResult::Started);
    if (result ==
        audio_playback::FileStartResult::RejectedPreservingPlayback) {
      return result;
    }
    if (result ==
        audio_playback::FileStartResult::FailedAfterReplacingPlayback) {
      snapshot_.ready = false;
      snapshot_.source.reset();
      return result;
    }
    startedFiles.push_back(file);
    startedTrackIndices.push_back(trackIndex);
    snapshot_.source = AudioPlaybackSource{file, trackIndex};
    snapshot_.ready = true;
    return audio_playback::FileStartResult::Started;
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
  audio_playback::FileStartResult nextStartResult =
      audio_playback::FileStartResult::Started;
  std::vector<std::filesystem::path> attemptedFiles;
  std::vector<std::filesystem::path> startedFiles;
  std::vector<int> startedTrackIndices;
  std::optional<double> lastSeekRatio;

 private:
  AudioPlaybackSnapshot snapshot_;
};

enum class VideoOpenPlan {
  ReadyNow,
  ReadyOnPump,
  CancelOnPump,
  CancelNow,
  AudioFallbackNow,
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
      case VideoOpenPlan::CancelOnPump:
        record_->opening = true;
        return std::nullopt;
      case VideoOpenPlan::CancelNow:
        return playback_session::OpenCancelled{};
      case VideoOpenPlan::AudioFallbackNow:
        return playback_session::OpenAudioFallback{
            {"No video stream was found.",
             "The media contains a playable audio stream."}};
    }
    std::abort();
  }
  std::optional<playback_session::OpenOutcome> pumpOpen() override {
    if (!record_->opening) return std::nullopt;
    record_->opening = false;
    if (record_->openPlan == VideoOpenPlan::CancelOnPump) {
      return playback_session::OpenCancelled{};
    }
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

playback_route::Route routeFor(const PlaybackTarget& target) {
  playback_route::Route route;
  route.target = target;
  return route;
}

playback_route::Route routeFor(const std::filesystem::path& file) {
  return routeFor(playbackFileTarget(file));
}

}  // namespace

int main() {
  bool ok = true;

  // Exercise the activation owner directly: the transaction type must make
  // rollback, one-shot commit and stale-token rejection observable without a
  // shell or presentation layer.
  {
    RecordingAudioSession endpoint;
    media_processing::Coordinator protocolProcessing(
        media_processing::Coordinator::Operations{});
    playback_queue::Queue protocolQueue(
        {[](const std::filesystem::path& file) {
           return std::optional<PlaybackTarget>(playbackFileTarget(file));
         },
         [](const PlaybackTarget& target) {
           playback_route::Route route;
           route.target = target;
           return route;
         }});
    application_playback::PlaybackControlRouter protocolControl(endpoint);
    application_playback::PlaybackActivationController protocol(
        protocolQueue, endpoint, protocolControl, protocolProcessing);

    const std::filesystem::path originalAudio = "protocol-original.flac";
    std::optional<playback_queue::Queue::PreparedActivation> original =
        protocolQueue.prepareStart(
            routeFor(originalAudio),
            playback_queue::singleSource(playbackFileTarget(originalAudio)));
    const application_playback::AudioActivationResult originalResult =
        protocol.activateAudio(std::move(*original));
    const PlaybackControlSessionId originalControl =
        protocolControl.sessionId();
    ok &= expect(
        std::holds_alternative<application_playback::PlaybackActivated>(
            originalResult) &&
            originalControl.valid(),
        "the activation owner must publish control only after audio commit");

    std::optional<application_playback::VideoActivationTransaction>
        firstTransition = protocol.beginVideoActivation();
    ok &= expect(firstTransition &&
                     !protocol.beginVideoActivation() &&
                     !protocolControl.sessionId().valid(),
                 "only one video activation transaction may suspend the "
                 "current control identity");

    std::optional<application_playback::VideoActivationTransaction>
        liveTransition(std::move(*firstTransition));
    const std::filesystem::path staleVideo = "stale-token.mp4";
    std::optional<playback_queue::Queue::PreparedActivation> staleCandidate =
        protocolQueue.prepareStart(
            routeFor(staleVideo),
            playback_queue::singleSource(playbackFileTarget(staleVideo)));
    ok &= expect(
        !protocol.activateReadyVideo(std::move(*firstTransition),
                                     std::move(*staleCandidate)) &&
            !protocolControl.sessionId().valid(),
        "a moved-from video transaction must not commit or restore playback");

    liveTransition.reset();
    const PlaybackControlSessionId restoredControl =
        protocolControl.sessionId();
    ok &= expect(restoredControl.valid() &&
                     restoredControl != originalControl &&
                     endpoint.snapshot().source &&
                     endpoint.snapshot().source->file == originalAudio,
                 "destroying the live transaction must restore exactly the "
                 "displaced audio under a fresh control identity");

    std::optional<application_playback::VideoActivationTransaction>
        mismatchedTransition = protocol.beginVideoActivation();
    (void)endpoint.startFile("unrelated-replacement.flac", 0);
    mismatchedTransition.reset();
    ok &= expect(!protocolControl.sessionId().valid(),
                 "rollback must not assign displaced ownership to a different "
                 "audio source");

    const std::filesystem::path recoveredAudio = "protocol-recovered.flac";
    std::optional<playback_queue::Queue::PreparedActivation> recovered =
        protocolQueue.prepareStart(
            routeFor(recoveredAudio),
            playback_queue::singleSource(playbackFileTarget(recoveredAudio)));
    ok &= expect(
        std::holds_alternative<application_playback::PlaybackActivated>(
            protocol.activateAudio(std::move(*recovered))),
        "a completed rollback must allow a later audio activation");

    const std::filesystem::path committedVideo = "protocol-video.mp4";
    const std::filesystem::path videoSuccessor = "protocol-successor.mp4";
    std::optional<application_playback::VideoActivationTransaction>
        committedTransition = protocol.beginVideoActivation();
    std::optional<playback_queue::Queue::PreparedActivation> videoCandidate =
        protocolQueue.prepareStart(
            routeFor(committedVideo),
            playback_queue::sourceFromFiles(
                {committedVideo, videoSuccessor}));
    const std::optional<application_playback::PlaybackActivated>
        videoActivated = protocol.activateReadyVideo(
            std::move(*committedTransition), std::move(*videoCandidate));
    const PlaybackControlSessionId videoControl = protocolControl.sessionId();
    ok &= expect(videoActivated && videoControl.valid(),
                 "a matching video transaction must commit queue and control "
                 "ownership together");

    const std::filesystem::path doubleCommit = "double-commit.mp4";
    std::optional<playback_queue::Queue::PreparedActivation> doubleCandidate =
        protocolQueue.prepareStart(
            routeFor(doubleCommit),
            playback_queue::singleSource(playbackFileTarget(doubleCommit)));
    const bool doubleCommitRejected =
        !protocol.activateReadyVideo(std::move(*committedTransition),
                                     std::move(*doubleCandidate));
    std::optional<playback_queue::Queue::PreparedActivation> adjacent =
        protocolQueue.prepareTransport(playback_queue::Direction::Next);
    ok &= expect(doubleCommitRejected &&
                     protocolControl.sessionId() == videoControl && adjacent &&
                     playbackTargetFile(adjacent->route().target) ==
                         videoSuccessor,
                 "a consumed transaction must reject double commit without "
                 "mutating the committed queue or control identity");
  }

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
  const PlaybackTarget firstAudioTrack =
      PlaybackTrackTarget{firstAudio, 7};
  ok &= expect(
      coordinator.startPlayback(
          routeFor(firstAudioTrack),
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
                   audio.startedTrackIndices == std::vector<int>{7} &&
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
  const std::filesystem::path rejectedAudio = "rejected.flac";
  const std::filesystem::path uncommittedAudioSuccessor =
      "not-current.flac";
  playback_route::Route rejectedAudioRoute = routeFor(rejectedAudio);
  rejectedAudioRoute.audioPictureInPicture =
      playback_route::AudioPictureInPicturePlan::Close;
  audio.nextStartResult =
      audio_playback::FileStartResult::RejectedPreservingPlayback;
  ok &= expect(
      !coordinator.startPlayback(
          std::move(rejectedAudioRoute),
          playback_queue::sourceFromFiles(
              {rejectedAudio, uncommittedAudioSuccessor})) &&
          audio.attemptedFiles.back() == rejectedAudio &&
          audio.startedFiles == std::vector<std::filesystem::path>{
                                    firstAudio, secondAudio} &&
          audio.snapshot().source &&
          audio.snapshot().source->file == secondAudio &&
          coordinator.controlSessionId() == secondAudioControlSession &&
          !queue.prepareTransport(playback_queue::Direction::Next),
      "an endpoint rejection that preserves current playback must also "
      "preserve its queue and control identity");
  const TuiMediaCoordinator::PollResult rejectedAudioResult =
      coordinator.poll();
  const bool reportedRejectedAudio = std::any_of(
      rejectedAudioResult.events.begin(), rejectedAudioResult.events.end(),
      [&](const TuiMediaCoordinator::Event& event) {
        const auto* failure =
            std::get_if<TuiMediaCoordinator::AudioPlaybackFailed>(&event);
        return failure && failure->file == rejectedAudio;
      });
  const bool appliedRejectedAudioPresentation = std::any_of(
      rejectedAudioResult.events.begin(), rejectedAudioResult.events.end(),
      [](const TuiMediaCoordinator::Event& event) {
        const auto* applied =
            std::get_if<TuiMediaCoordinator::ApplyAudioPictureInPicture>(
                &event);
        return applied &&
               applied->plan ==
                   playback_route::AudioPictureInPicturePlan::Close;
      });
  ok &= expect(reportedRejectedAudio && !appliedRejectedAudioPresentation,
               "audio endpoint rejection must publish its typed source "
               "failure without applying the uncommitted route presentation");

  const std::filesystem::path rejectedVideo = "rejected.mp4";
  playback_route::Route rejectedVideoRoute = routeFor(rejectedVideo);
  rejectedVideoRoute.audioPictureInPicture =
      playback_route::AudioPictureInPicturePlan::Close;
  rejectNextVideoSession = true;
  ok &= expect(
      coordinator.startPlayback(
          std::move(rejectedVideoRoute),
          playback_queue::singleSource(playbackFileTarget(rejectedVideo))) &&
          !coordinator.videoReady() && audio.snapshot().source &&
          audio.snapshot().source->file == secondAudio &&
          coordinator.controlSessionId().valid() &&
          coordinator.controlSessionId() != secondAudioControlSession &&
          !queue.prepareTransport(playback_queue::Direction::Next),
      "a rejected video transition must restore control ownership for audio "
      "that the endpoint left intact");
  const PlaybackControlSessionId restoredAudioControlSession =
      coordinator.controlSessionId();
  const TuiMediaCoordinator::PollResult rejectedVideoResult =
      coordinator.poll();
  const bool reportedRejectedVideo = std::any_of(
      rejectedVideoResult.events.begin(), rejectedVideoResult.events.end(),
      [&](const TuiMediaCoordinator::Event& event) {
        const auto* failure =
            std::get_if<TuiMediaCoordinator::VideoPlaybackFailed>(&event);
        return failure && failure->file == rejectedVideo;
      });
  const bool appliedRejectedVideoPresentation = std::any_of(
      rejectedVideoResult.events.begin(), rejectedVideoResult.events.end(),
      [](const TuiMediaCoordinator::Event& event) {
        const auto* applied =
            std::get_if<TuiMediaCoordinator::ApplyAudioPictureInPicture>(
                &event);
        return applied &&
               applied->plan ==
                   playback_route::AudioPictureInPicturePlan::Close;
      });
  ok &= expect(reportedRejectedVideo && !appliedRejectedVideoPresentation,
               "rejected video activation must publish its typed failure "
               "without applying uncommitted presentation state");

  ok &= expect(
      !coordinator.handleSystemControlCommand(
          {secondAudioControlSession, PlaybackControlCommand::Play}) &&
          coordinator.handleSystemControlCommand(
              {restoredAudioControlSession, PlaybackControlCommand::Stop}) &&
          audio.playRequests == 0 && audio.stopRequests == 1 &&
          !coordinator.controlSessionId().valid(),
      "stopping audio must retire its identity without accepting commands "
      "from the previous queue item");

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

  const std::filesystem::path declinedFallback = "declined-fallback.mkv";
  const std::filesystem::path declinedFallbackSuccessor =
      "declined-successor.flac";
  nextVideoOpenPlan = VideoOpenPlan::AudioFallbackNow;
  ok &= expect(
      coordinator.startPlayback(
          routeFor(declinedFallback),
          playback_queue::sourceFromFiles(
              {declinedFallback, declinedFallbackSuccessor})) &&
          !coordinator.videoReady() &&
          !coordinator.controlSessionId().valid() &&
          !coordinator.canAcceptExternalMediaChange(),
      "an audio fallback offer must retain its activation without exposing "
      "partial playback state");
  const TuiMediaCoordinator::PollResult declinedFallbackResult =
      coordinator.poll();
  std::optional<application_playback::AudioFallbackDecisionId>
      declinedDecision;
  for (const TuiMediaCoordinator::Event& event :
       declinedFallbackResult.events) {
    if (const auto* request =
            std::get_if<application_playback::AudioFallbackRequest>(&event)) {
      if (request->file == declinedFallback) {
        declinedDecision = request->id;
      }
    }
  }
  ok &= expect(
      declinedDecision &&
          !coordinator.resolveAudioFallback(
              {declinedDecision->value + 1}, true) &&
          coordinator.resolveAudioFallback(*declinedDecision, false) &&
          !coordinator.controlSessionId().valid() &&
          audio.startedFiles == std::vector<std::filesystem::path>{
                                    firstAudio, secondAudio} &&
          !queue.prepareTransport(playback_queue::Direction::Next),
      "only the matching fallback decision may resolve the retained "
      "activation, and declining it must discard that activation");

  const std::filesystem::path revokedFallback = "revoked-fallback.mkv";
  nextVideoOpenPlan = VideoOpenPlan::AudioFallbackNow;
  ok &= expect(
      coordinator.startPlayback(
          routeFor(revokedFallback),
          playback_queue::singleSource(
              playbackFileTarget(revokedFallback))) &&
          !coordinator.canAcceptExternalMediaChange(),
      "a fallback pending during quit must remain an identifiable domain "
      "request");
  const TuiMediaCoordinator::PollResult revocableFallbackResult =
      coordinator.poll();
  std::optional<application_playback::AudioFallbackDecisionId>
      revocableDecision;
  for (const TuiMediaCoordinator::Event& event :
       revocableFallbackResult.events) {
    if (const auto* request =
            std::get_if<application_playback::AudioFallbackRequest>(&event)) {
      if (request->file == revokedFallback) {
        revocableDecision = request->id;
      }
    }
  }
  coordinator.requestQuit();
  const TuiMediaCoordinator::PollResult revokedFallbackResult =
      coordinator.poll();
  const bool fallbackRevoked = std::any_of(
      revokedFallbackResult.events.begin(),
      revokedFallbackResult.events.end(),
      [&](const TuiMediaCoordinator::Event& event) {
        const auto* revoked =
            std::get_if<application_playback::AudioFallbackRevoked>(&event);
        return revoked && revocableDecision &&
               revoked->id == *revocableDecision;
      });
  ok &= expect(revocableDecision && fallbackRevoked &&
                   !coordinator.resolveAudioFallback(*revocableDecision,
                                                     true) &&
                   coordinator.canAcceptExternalMediaChange(),
               "quit must revoke the published fallback identity and discard "
               "its hidden activation");

  const std::filesystem::path acceptedFallback = "accepted-fallback.mkv";
  playback_route::Route acceptedFallbackRoute = routeFor(acceptedFallback);
  acceptedFallbackRoute.audioPictureInPicture =
      playback_route::AudioPictureInPicturePlan::Close;
  nextVideoOpenPlan = VideoOpenPlan::AudioFallbackNow;
  ok &= expect(
      coordinator.startPlayback(
          std::move(acceptedFallbackRoute),
          playback_queue::singleSource(
              playbackFileTarget(acceptedFallback))) &&
          !coordinator.videoReady(),
      "a later fallback workflow must remain available after a decline");
  const TuiMediaCoordinator::PollResult acceptedFallbackResult =
      coordinator.poll();
  std::optional<application_playback::AudioFallbackDecisionId>
      acceptedDecision;
  for (const TuiMediaCoordinator::Event& event :
       acceptedFallbackResult.events) {
    if (const auto* request =
            std::get_if<application_playback::AudioFallbackRequest>(&event)) {
      if (request->file == acceptedFallback) {
        acceptedDecision = request->id;
      }
    }
  }
  const bool fallbackPresentationAppliedBeforeDecision = std::any_of(
      acceptedFallbackResult.events.begin(),
      acceptedFallbackResult.events.end(),
      [](const TuiMediaCoordinator::Event& event) {
        return std::holds_alternative<
            TuiMediaCoordinator::ApplyAudioPictureInPicture>(event);
      });
  const bool acceptedFallbackActivated =
      declinedDecision && acceptedDecision &&
          *acceptedDecision != *declinedDecision &&
          !coordinator.resolveAudioFallback(*declinedDecision, true) &&
          coordinator.resolveAudioFallback(*acceptedDecision, true) &&
          audio.startedFiles.back() == acceptedFallback &&
          coordinator.controlSessionId().valid();
  const TuiMediaCoordinator::PollResult acceptedFallbackActivation =
      coordinator.poll();
  const bool appliedCommittedFallbackPresentation = std::any_of(
      acceptedFallbackActivation.events.begin(),
      acceptedFallbackActivation.events.end(),
      [](const TuiMediaCoordinator::Event& event) {
        const auto* applied =
            std::get_if<TuiMediaCoordinator::ApplyAudioPictureInPicture>(
                &event);
        return applied &&
               applied->plan ==
                   playback_route::AudioPictureInPicturePlan::Close;
      });
  ok &= expect(
      acceptedFallbackActivated &&
          !fallbackPresentationAppliedBeforeDecision &&
          appliedCommittedFallbackPresentation,
      "accepting the matching fallback must atomically activate audio, "
      "commit the queue, publish control ownership and then apply route "
      "presentation");
  const PlaybackControlSessionId fallbackControlSession =
      coordinator.controlSessionId();

  const std::filesystem::path asyncCancelledVideo =
      "async-cancelled-video.mp4";
  const std::filesystem::path asyncCancelledSuccessor =
      "async-cancelled-successor.mp4";
  nextVideoOpenPlan = VideoOpenPlan::CancelOnPump;
  ok &= expect(
      coordinator.startPlayback(
          routeFor(asyncCancelledVideo),
          playback_queue::sourceFromFiles(
              {asyncCancelledVideo, asyncCancelledSuccessor})) &&
          !coordinator.videoReady() &&
          !coordinator.controlSessionId().valid() &&
          audio.snapshot().source &&
          audio.snapshot().source->file == acceptedFallback,
      "an asynchronous video open must suspend, but not misidentify, the "
      "currently audible source");
  const TuiMediaCoordinator::PollResult asyncCancellation =
      coordinator.poll();
  const PlaybackControlSessionId restoredAfterAsyncCancel =
      coordinator.controlSessionId();
  ok &= expect(
      asyncCancellation.playbackChanged && !coordinator.videoReady() &&
          restoredAfterAsyncCancel.valid() &&
          restoredAfterAsyncCancel != fallbackControlSession &&
          audio.snapshot().source &&
          audio.snapshot().source->file == acceptedFallback &&
          !coordinator.handleSystemControlCommand(
              {fallbackControlSession, PlaybackControlCommand::Pause}) &&
          !queue.prepareTransport(playback_queue::Direction::Next),
      "asynchronous cancellation must destroy the opening session before "
      "restoring the exact displaced audio with a fresh control identity");

  const std::filesystem::path destructiveAudioFailure =
      "decoder-failure.flac";
  const std::filesystem::path destructiveFailureSuccessor =
      "decoder-failure-successor.flac";
  audio.nextStartResult =
      audio_playback::FileStartResult::FailedAfterReplacingPlayback;
  ok &= expect(
      !coordinator.startPlayback(
          routeFor(destructiveAudioFailure),
          playback_queue::sourceFromFiles(
              {destructiveAudioFailure, destructiveFailureSuccessor})) &&
          !audio.snapshot().ready &&
          !audio.snapshot().source &&
          !coordinator.controlSessionId().valid() &&
          !coordinator.handleSystemControlCommand(
              {restoredAfterAsyncCancel, PlaybackControlCommand::Pause}) &&
          !coordinator.handleControlCommand(PlaybackControlCommand::Next) &&
          !queue.prepareTransport(playback_queue::Direction::Next),
      "an endpoint failure after replacement begins must retire the old "
      "source and identity without activating its queued successor");

  const std::filesystem::path thirdVideo = "third.mp4";
  const std::size_t sessionsBeforeAsyncOpen = sessions.size();
  nextVideoOpenPlan = VideoOpenPlan::ReadyOnPump;
  ok &= expect(
      coordinator.startPlayback(routeFor(thirdVideo),
                                playback_queue::singleSource(
                                    playbackFileTarget(thirdVideo))) &&
          !coordinator.videoReady() &&
          sessions.size() == sessionsBeforeAsyncOpen + 1 &&
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
          !coordinator.controlSessionId().valid() &&
          !coordinator.handleControlCommand(PlaybackControlCommand::Pause) &&
          sessions.back()->controlCommands ==
              std::vector<PlaybackControlCommand>{
                  PlaybackControlCommand::Stop},
      "video stop must reach the active endpoint before retiring its control "
      "identity, and later direct commands must remain fenced out");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
