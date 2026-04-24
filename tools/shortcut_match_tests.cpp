#include "playback/ascii/frame_output.h"
#include "playback/input/shortcuts.h"
#include "playback/overlay/overlay.h"
#include "playback/video/audio_clock_reacquire.h"
#include "playback/video/sync.h"
#include "playback/video/state_machine.h"
#include "playback/video/video_enhancement.h"
#include "playback/session/presentation_policy.h"
#include "playback/session/window_presentation.h"
#include "clock.h"

#include <iostream>
#include <string>

namespace {

KeyEvent makeKey(WORD vk, char ch = 0, DWORD control = 0) {
  KeyEvent key{};
  key.vk = vk;
  key.ch = ch;
  key.control = control;
  return key;
}

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "shortcut_match_tests: " << message << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bool ok = true;

  ok &= expect(!matchesShortcut(makeKey(VK_F1), VK_ESCAPE, 0, 0),
               "VK_F1 must not match VK_ESCAPE when no ASCII fallback exists");
  ok &= expect(!matchesShortcut(makeKey(VK_F1), VK_BACK, 0, 0),
               "VK_F1 must not match VK_BACK when no ASCII fallback exists");
  ok &= expect(!resolvePlaybackShortcutAction(
                   makeKey(VK_F1), kPlaybackShortcutContextPlaybackSession)
                   .has_value(),
               "VK_F1 must not resolve to any playback-session shortcut");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_ESCAPE), kPlaybackShortcutContextPlaybackSession)
                   .value() == PlaybackShortcutAction::ExitPlaybackSession,
               "VK_ESCAPE must still exit playback session");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_ESCAPE), kPlaybackShortcutContextImageViewer)
                   .value() == PlaybackShortcutAction::CloseViewer,
               "VK_ESCAPE must still close the image viewer");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_LEFT), kPlaybackShortcutContextShared)
                   .value() == PlaybackShortcutAction::SeekBackward,
               "VK_LEFT must seek backward");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_RIGHT), kPlaybackShortcutContextShared)
                   .value() == PlaybackShortcutAction::SeekForward,
               "VK_RIGHT must seek forward");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_LEFT),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextImageViewer)
                   .value() == PlaybackShortcutAction::SeekBackward,
               "Image viewer must inherit the shared bare-arrow seek layer");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_RIGHT),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextImageViewer)
                   .value() == PlaybackShortcutAction::SeekForward,
               "Image viewer must inherit the shared bare-arrow seek layer");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_LEFT, 0, kPlaybackShortcutCtrlMask),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextImageViewer)
                   .value() == PlaybackShortcutAction::Previous,
               "Ctrl+VK_LEFT must navigate to the previous item");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_RIGHT, 0, kPlaybackShortcutCtrlMask),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextImageViewer)
                   .value() == PlaybackShortcutAction::Next,
               "Ctrl+VK_RIGHT must navigate to the next item");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_RETURN, 0, kPlaybackShortcutAltMask),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextPlaybackSession)
                   .value() == PlaybackShortcutAction::ToggleFullscreen,
               "Alt+Enter must resolve to the fullscreen toggle");
  ok &= expect(!resolvePlaybackShortcutAction(
                   makeKey(VK_RETURN, 0, kPlaybackShortcutAltMask),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextImageViewer),
               "Alt+Enter must be playback-session scoped");
  ok &= expect(planFullscreenToggle({PlaybackPresentationFamily::Ascii,
                                     PlaybackPresentationMode::
                                         DefaultNonFullscreen})
                   .target == PlaybackPresentationMode::Fullscreen,
               "ASCII default presentation must enter fullscreen");
  ok &= expect(planFullscreenToggle({PlaybackPresentationFamily::Ascii,
                                     PlaybackPresentationMode::Fullscreen})
                   .target ==
                   PlaybackPresentationMode::DefaultNonFullscreen,
               "ASCII fullscreen must exit to the default non-fullscreen "
               "presentation");
  ok &= expect(planFullscreenToggle({PlaybackPresentationFamily::Ascii,
                                     PlaybackPresentationMode::PictureInPicture})
                   .target == PlaybackPresentationMode::Fullscreen,
               "ASCII PiP must enter fullscreen");
  ok &= expect(planFullscreenToggle({PlaybackPresentationFamily::Framebuffer,
                                     PlaybackPresentationMode::Fullscreen})
                   .target == PlaybackPresentationMode::PictureInPicture,
               "Framebuffer fullscreen must exit to PiP");
  ok &= expect(planFullscreenToggle({PlaybackPresentationFamily::Framebuffer,
                                     PlaybackPresentationMode::PictureInPicture})
                   .target == PlaybackPresentationMode::Fullscreen,
               "Framebuffer PiP must enter fullscreen");
  PlaybackWindowPlacementState fullscreenPlacement{};
  fullscreenPlacement.fullscreenActive = true;
  ok &= expect(playback_window_presentation::shouldStartFullscreen(
                   fullscreenPlacement),
               "fullscreen placement must start the next window fullscreen");
  PlaybackWindowPlacementState windowPlacement{};
  ok &= expect(!playback_window_presentation::shouldStartFullscreen(
                   windowPlacement),
               "windowed placement must start the next window windowed");
  PlaybackWindowPlacementState fullscreenRestoredPipPlacement{};
  fullscreenRestoredPipPlacement.pictureInPictureActive = true;
  fullscreenRestoredPipPlacement.pictureInPictureRestoreFullscreen = true;
  ok &= expect(playback_window_presentation::shouldStartFullscreen(
                   fullscreenRestoredPipPlacement),
               "PiP that restores fullscreen must start from fullscreen");
  PlaybackWindowPlacementState windowRestoredPipPlacement{};
  windowRestoredPipPlacement.pictureInPictureActive = true;
  ok &= expect(!playback_window_presentation::shouldStartFullscreen(
                   windowRestoredPipPlacement),
               "PiP without fullscreen restore must not start fullscreen");
  ok &= expect(playback_overlay::overlayCellCountForPixels(719, 21) == 35,
               "overlayCellCountForPixels must round rows up");
  ok &= expect(playback_overlay::overlayCellCountForPixels(960, 9) == 107,
               "overlayCellCountForPixels must round columns up");
  ok &= expect(playback_frame_output::centerContentTop(0, 30, 20) == 5,
               "centerContentTop must center smaller content vertically");
  ok &= expect(playback_frame_output::centerContentTop(4, 30, 30) == 4,
               "centerContentTop must keep full-height content anchored");
  ok &= expect(playback_frame_output::centerContentTop(2, 30, 40) == 2,
               "centerContentTop must not shift oversized content");

  playback_video_enhancement::VideoEnhancementPipeline enhancementPipeline;
  VideoFrame enhancementInput{};
  enhancementInput.format = VideoPixelFormat::HWTexture;
  playback_video_enhancement::VideoEnhancementRequest enhancementRequest;
  enhancementRequest.input = &enhancementInput;
  enhancementRequest.consumer =
      playback_video_enhancement::VideoEnhancementConsumer::FramebufferVideo;
  enhancementRequest.frameChanged = true;
  playback_video_enhancement::VideoEnhancementResult enhancementResult =
      enhancementPipeline.process(enhancementRequest);
  ok &= expect(enhancementResult.frame == &enhancementInput &&
                   enhancementResult.frameAvailable &&
                   enhancementResult.frameChanged &&
                   !enhancementResult.enhanced,
               "Video enhancement pipeline must pass frames through before an "
               "enhancement backend can process the frame");
#if defined(_WIN32) && defined(RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO) && \
    RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO
  const std::string expectedEnhancementBackend =
      "nvidia-d3d11-video-processor";
#else
  const std::string expectedEnhancementBackend = "none";
#endif
  ok &= expect(std::string(enhancementPipeline.backendName()) ==
                   expectedEnhancementBackend,
               "Video enhancement pipeline must expose the active backend");

  enhancementRequest.consumer =
      playback_video_enhancement::VideoEnhancementConsumer::TextGridInput;
  enhancementRequest.frameChanged = false;
  enhancementRequest.forceRefresh = true;
  playback_video_enhancement::VideoEnhancementResult textGridEnhancement =
      enhancementPipeline.process(enhancementRequest);
  ok &= expect(textGridEnhancement.frame == &enhancementInput &&
                   textGridEnhancement.frameChanged,
               "Text-grid input must use the same enhancement pipeline");

  playback_state_machine::PipelineSnapshot pausedSeekBeforeFrame{};
  pausedSeekBeforeFrame.audioPaused = true;
  pausedSeekBeforeFrame.currentSerial = 2;
  pausedSeekBeforeFrame.lastPresentedSerial = 1;
  pausedSeekBeforeFrame.pendingSeekSerial = 2;
  pausedSeekBeforeFrame.seekInFlightSerial = 0;

  playback_state_machine::PipelineSnapshot pausedSeekAfterFrame =
      pausedSeekBeforeFrame;
  pausedSeekAfterFrame.lastPresentedSerial = 2;
  pausedSeekAfterFrame.pendingSeekSerial = 0;

  playback_state_machine::PipelineSnapshot failedPausedSeek =
      pausedSeekBeforeFrame;
  failedPausedSeek.seekFailed = true;
  failedPausedSeek.pendingSeekSerial = 0;

  playback_state_machine::PipelineSnapshot playingSeek = pausedSeekBeforeFrame;
  playingSeek.audioPaused = false;

  playback_state_machine::PipelineSnapshot openingReady{};
  openingReady.initDone = true;
  openingReady.initOk = true;
  openingReady.videoQueueDepth = 1;
  openingReady.videoQueueEmpty = false;
  auto putControllerInSeeking = [&](playback_state_machine::Controller& c) {
    c.beginOpening();
    c.observe(openingReady, 1);
    c.finishPriming();
    c.beginSeeking();
  };

  playback_state_machine::Controller pausedSeekPendingController;
  putControllerInSeeking(pausedSeekPendingController);
  playback_state_machine::Evaluation pausedSeekPending =
      pausedSeekPendingController.observe(pausedSeekBeforeFrame, 1);
  ok &= expect(!pausedSeekPending.change.changed &&
                   pausedSeekPending.change.current == PlayerState::Seeking,
               "Paused seek must remain Seeking while a preview frame is "
               "pending");

  playback_state_machine::Controller pausedSeekDoneController;
  putControllerInSeeking(pausedSeekDoneController);
  playback_state_machine::Evaluation pausedSeekDone =
      pausedSeekDoneController.observe(pausedSeekAfterFrame, 1);
  ok &= expect(pausedSeekDone.change.changed &&
                   pausedSeekDone.change.current == PlayerState::Paused,
               "Paused seek must settle back to Paused after the preview frame "
               "is shown");

  playback_state_machine::Controller failedSeekController;
  putControllerInSeeking(failedSeekController);
  playback_state_machine::Evaluation failedSeek =
      failedSeekController.observe(failedPausedSeek, 1);
  ok &= expect(failedSeek.change.changed &&
                   failedSeek.change.current == PlayerState::Paused,
               "Failed paused seek must fall back to Paused");

  playback_state_machine::Controller playingSeekController;
  putControllerInSeeking(playingSeekController);
  playback_state_machine::Evaluation playingSeekDone =
      playingSeekController.observe(playingSeek, 1);
  ok &= expect(playingSeekDone.change.changed &&
                   playingSeekDone.change.current == PlayerState::Prefill,
               "Active seeks must continue into Prefill");
  ok &= expect(playback_state_machine::stateEffects(PlayerState::Seeking)
                   .holdAudioOutput,
               "Seeking must hold audio until clocks are reacquired");
  ok &= expect(!playback_state_machine::stateEffects(PlayerState::Playing)
                    .holdAudioOutput,
               "Playing must release audio output hold");
  ok &= expect(playback_state_machine::stateEffects(PlayerState::Paused)
                   .pauseMainClock,
               "Paused state must pause the main clock");
  ok &= expect(!playback_state_machine::stateEffects(PlayerState::Playing)
                    .pauseMainClock,
               "Playing state must run the main clock");
  playback_state_machine::StateProjection playingProjection =
      playback_state_machine::project(PlayerState::Playing);
  ok &= expect(
      playingProjection.session ==
              playback_state_machine::SessionState::Started &&
          playingProjection.transport ==
              playback_state_machine::TransportState::Playing &&
          playingProjection.buffering ==
              playback_state_machine::BufferingState::Ready,
      "Playing must project to started/playing/ready");
  ok &= expect(playingProjection.effects.mayPresentVideo,
               "Ready playback must allow video presentation");
  playback_state_machine::StateProjection prefillProjection =
      playback_state_machine::project(PlayerState::Prefill);
  ok &= expect(prefillProjection.effects.holdAudioOutput &&
                   prefillProjection.effects.pauseMainClock &&
                   !prefillProjection.effects.mayPresentVideo,
               "Prefill must hold outputs until both streams are ready");
  ok &= expect(pausedSeekDone.change.current == PlayerState::Paused &&
                   pausedSeekDone.clearSeekFailure,
               "Leaving Seeking after seek completion must clear seek failure "
               "state centrally");
  playback_state_machine::Controller stateController;
  playback_state_machine::StateChange openingChange =
      stateController.beginOpening();
  ok &= expect(openingChange.changed &&
                   openingChange.previous == PlayerState::Idle &&
                   openingChange.current == PlayerState::Opening &&
                   openingChange.projection.session ==
                       playback_state_machine::SessionState::Opening,
               "State controller must own the Opening transition");
  playback_state_machine::StateChange earlySeek =
      stateController.beginSeeking();
  ok &= expect(!earlySeek.changed && earlySeek.current == PlayerState::Opening,
               "Seeking must not start before the session is started");
  playback_state_machine::Evaluation observedOpening =
      stateController.observe(openingReady, 1);
  ok &= expect(observedOpening.change.changed &&
                   observedOpening.change.current == PlayerState::Priming,
               "Observed pipeline readiness must update the controller");
  playback_state_machine::StateChange seekChange =
      stateController.beginSeeking();
  ok &= expect(seekChange.changed &&
                   seekChange.current == PlayerState::Seeking &&
                   seekChange.projection.transport ==
                       playback_state_machine::TransportState::Seeking,
               "State controller must own the Seeking transition");
  playback_state_machine::StateChange invalidPrimingFinish =
      stateController.finishPriming();
  ok &= expect(!invalidPrimingFinish.changed &&
                   invalidPrimingFinish.current == PlayerState::Seeking,
               "Priming completion must be ignored outside Priming");
  playback_state_machine::Evaluation observedSeek =
      stateController.observe(playingSeek, 1);
  ok &= expect(observedSeek.change.changed &&
                   observedSeek.change.current == PlayerState::Prefill,
               "Observed active seek completion must enter Prefill");
  playback_state_machine::Evaluation observedPrefill =
      stateController.observe(openingReady, 1);
  ok &= expect(observedPrefill.change.changed &&
                   observedPrefill.change.current == PlayerState::Priming,
               "Observed ready prefill must enter Priming");
  playback_state_machine::StateChange primingFinish =
      stateController.finishPriming();
  ok &= expect(primingFinish.changed &&
                   primingFinish.current == PlayerState::Playing &&
                   primingFinish.projection.buffering ==
                       playback_state_machine::BufferingState::Ready,
               "State controller must own the priming-to-playing transition");

  playback_sync::LoopState loopState{};
  loopState.frameTimerUs = 1000;
  playback_sync::PreparedFrame prepared{};
  prepared.frame.ptsUs = 5000;
  prepared.frame.durationUs = 33333;
  playback_main_clock::Snapshot master{};
  playback_sync::FramePlan seekPlan = playback_sync::planFrame(
      loopState, PlayerState::Seeking, prepared, master, 8000);
  ok &= expect(seekPlan.delayUs == 0 && seekPlan.targetUs == 8000,
               "Seeking must present the next frame immediately");

  playback_main_clock::Controller mainClock;
  playback_main_clock::SampleRequest clockRequest{};
  clockRequest.currentSerial = 4;
  clockRequest.nowUs = 1000000;
  clockRequest.audio.active = true;
  clockRequest.audio.mayDriveMaster = true;
  clockRequest.audio.ready = true;
  clockRequest.audio.fresh = true;
  clockRequest.audio.serial = 4;
  clockRequest.audio.us = 10000;
  clockRequest.video.ready = true;
  clockRequest.video.serial = 4;
  clockRequest.video.us = 9000;
  playback_main_clock::Snapshot audioMaster = mainClock.sample(clockRequest);
  ok &= expect(audioMaster.source == PlayerClockSource::Audio,
               "Eligible audio must be selected as the main playback clock");
  ok &= expect(playback_main_clock::convertToSystemUs(audioMaster, 20000, 0) ==
                   1010000,
               "Main clock must convert stream timestamps to system time");

  clockRequest.audio.mayDriveMaster = false;
  playback_main_clock::Snapshot videoFallback = mainClock.sample(clockRequest);
  ok &= expect(videoFallback.source == PlayerClockSource::Video,
               "Blocked audio master must fall back to the video clock");

  playback_sync::LoopState convertedLoop{};
  convertedLoop.frameTimerUs = 500000;
  playback_sync::PreparedFrame convertedFrame{};
  convertedFrame.frame.ptsUs = 20000;
  convertedFrame.frame.durationUs = 33333;
  convertedFrame.delayUs = 33333;
  convertedFrame.actualDurationUs = 33333;
  playback_sync::FramePlan convertedPlan = playback_sync::planFrame(
      convertedLoop, PlayerState::Playing, convertedFrame, audioMaster, 1000000);
  ok &= expect(convertedPlan.targetUs == 1010000,
               "Video scheduling must use the main clock's stream-to-system "
               "conversion");

  playback_sync::LoopState driftLoop{};
  driftLoop.frameTimerUs = 700000;
  driftLoop.lastDelayUsValue = 33333;
  for (int i = 1; i <= 5; ++i) {
    playback_main_clock::Snapshot movingAudioMaster = audioMaster;
    movingAudioMaster.us = 10000 + (i - 1) * 33333;
    movingAudioMaster.systemUs = 1000000 + (i - 1) * 33333;

    playback_sync::PreparedFrame driftFrame{};
    driftFrame.frame.ptsUs = 10000 + i * 33333;
    driftFrame.frame.durationUs = 33333;
    driftFrame.delayUs = 33333;
    driftFrame.actualDurationUs = 33333;
    playback_sync::FramePlan driftPlan = playback_sync::planFrame(
        driftLoop, PlayerState::Playing, driftFrame, movingAudioMaster,
        movingAudioMaster.systemUs);
    int64_t expectedTargetUs = 1000000 + i * 33333;
    ok &= expect(driftPlan.targetUs == expectedTargetUs,
                 "Audio-master scheduling must not accumulate local timer drift");
    playback_sync::notePresentedFrame(driftLoop, driftFrame, driftPlan.delayUs,
                                      driftPlan.targetUs,
                                      movingAudioMaster.systemUs);
  }

  Clock pauseClock;
  pauseClock.set(1000, 10000, 9);
  pauseClock.set_paused(true, 20000);
  ok &= expect(pauseClock.get(30000) == 11000,
               "Paused clocks must not advance while paused");
  pauseClock.set_paused(false, 30000);
  ok &= expect(pauseClock.get(30000) == 11000,
               "Resuming must preserve the paused clock point");
  ok &= expect(pauseClock.get(40000) == 21000,
               "Resumed clocks must advance from the resume time");

  mainClock.startSession(5);
  ok &= expect(!mainClock.videoStatus(5, 500000).ready,
               "Starting a session must reset the output clock");
  mainClock.updateVideo(5, 12000, 500000);
  ok &= expect(mainClock.videoStatus(5, 500000).us == 12000,
               "Main clock must own video clock updates");
  mainClock.changePause(true, 510000);
  ok &= expect(mainClock.videoStatus(5, 530000).us == 22000,
               "Main clock pause must freeze the video output clock");

  playback_audio_clock_reacquire::Gate audioGate;
  ok &= expect(audioGate.snapshot(1).audioMayDriveMaster,
               "Audio clock must drive by default");
  audioGate.require(3, 1000);
  ok &= expect(!audioGate.snapshot(3).audioMayDriveMaster,
               "Audio clock must be blocked while reacquiring current serial");
  ok &= expect(audioGate.snapshot(2).audioMayDriveMaster,
               "Stale reacquire state must not block another serial");
  audioGate.noteQueuedAudio(3, 999, 512);
  ok &= expect(!audioGate.snapshot(3).audioMayDriveMaster,
               "Pre-target audio must not release audio master");
  audioGate.noteQueuedAudio(3, 1000, 512);
  ok &= expect(audioGate.snapshot(3).audioMayDriveMaster,
               "Post-target queued audio must release audio master");

  return ok ? 0 : 1;
}
