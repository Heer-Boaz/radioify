#include "playback/ascii/frame_output.h"
#include "playback/input/shortcuts.h"
#include "playback/overlay/overlay.h"
#include "playback/video/audio/clock_reacquire.h"
#include "playback/video/control/events.h"
#include "playback/video/control/serial.h"
#include "playback/video/frame_cursor.h"
#include "playback/video/frame_step_source_cache.h"
#include "playback/video/frame_step_seek.h"
#include "playback/video/timing/sync.h"
#include "playback/video/timing/timeline.h"
#include "playback/video/state/machine.h"
#include "playback/video/enhancement/pipeline.h"
#include "playback/session/presentation_policy.h"
#include "playback/session/state.h"
#include "clock.h"
#include "queues.h"

#include <iostream>
#include <limits>
#include <optional>
#include <string>

namespace {

KeyEvent makeKey(WORD vk, char ch = 0, DWORD control = 0) {
  KeyEvent key{};
  key.vk = vk;
  key.ch = ch;
  key.control = control;
  return key;
}

MouseEvent makeMouse(DWORD buttonState) {
  MouseEvent mouse{};
  mouse.buttonState = buttonState;
  return mouse;
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

  ok &= expect(playback_session_state::toggleRequestsPause(
                   PlaybackSessionState::Active, false),
               "Active playback must toggle to pause");
  ok &= expect(!playback_session_state::toggleRequestsPause(
                   PlaybackSessionState::Paused, false),
               "Paused playback must toggle to play");
  ok &= expect(!playback_session_state::toggleRequestsPause(
                   PlaybackSessionState::Ended, false),
               "Ended playback must toggle to replay");
  ok &= expect(!playback_session_state::toggleRequestsPause(
                   PlaybackSessionState::Active, true),
               "A player that ended before session sync must toggle to replay");

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
                   makeKey('P', 0, kPlaybackShortcutCtrlMask),
                   kPlaybackShortcutContextShared)
                   .value() == PlaybackShortcutAction::TogglePictureInPicture,
               "Ctrl+P must toggle PiP from shared playback controls");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey('P', 0, kPlaybackShortcutCtrlMask),
                   kPlaybackShortcutContextPictureInPicture)
                   .value() == PlaybackShortcutAction::TogglePictureInPicture,
               "Ctrl+P must toggle PiP while the PiP window has focus");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey('P'), kPlaybackShortcutContextPictureInPicture)
                   .value() == PlaybackShortcutAction::DismissPictureInPicture,
               "Bare P must still dismiss the PiP window");
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
  ok &= expect(!resolvePlaybackShortcutAction(
                   makeKey(VK_OEM_PERIOD, '.'),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared),
               "Frame-step shortcuts must stay scoped to video playback");
  ok &= expect(!resolvePlaybackShortcutAction(
                   makeKey(VK_OEM_PERIOD, '.'),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextPictureInPicture),
               "Frame-step shortcuts must not leak into audio picture-in-picture");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_OEM_COMMA, ','),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextPlaybackSession |
                       kPlaybackShortcutContextVideoPlayback)
                   .value() == PlaybackShortcutAction::PreviousFrame,
               "Comma must step to the previous video frame in video playback");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(VK_OEM_PERIOD, '.'),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextPlaybackSession |
                       kPlaybackShortcutContextVideoPlayback)
                   .value() == PlaybackShortcutAction::NextFrame,
               "Period must step to the next video frame in video playback");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey(0, '.'),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextPlaybackSession |
                       kPlaybackShortcutContextVideoPlayback)
                   .value() == PlaybackShortcutAction::NextFrame,
               "Terminal text input must also resolve period as next-frame");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey('S', 's'), kPlaybackShortcutContextShared)
                   .value() == PlaybackShortcutAction::ToggleSubtitles,
               "Bare S must continue to toggle subtitles");
  ok &= expect(!resolvePlaybackShortcutAction(
                   makeKey('S', 'S', SHIFT_PRESSED),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared),
               "Frame-copy must stay scoped to video playback");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey('S', 'S', SHIFT_PRESSED),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextPlaybackSession |
                       kPlaybackShortcutContextVideoPlayback)
                   .value() == PlaybackShortcutAction::CopyVideoFrame,
               "Shift+S must copy the current rendered video frame");
  ok &= expect(!resolvePlaybackShortcutAction(
                   makeKey('S', 'S', SHIFT_PRESSED | LEFT_CTRL_PRESSED),
                   kPlaybackShortcutContextGlobal |
                       kPlaybackShortcutContextShared |
                       kPlaybackShortcutContextPlaybackSession |
                   kPlaybackShortcutContextVideoPlayback),
               "Ctrl+Shift+S must remain available for future shortcut layers");

  const uint32_t videoPlaybackContexts =
      kPlaybackShortcutContextGlobal | kPlaybackShortcutContextShared |
      kPlaybackShortcutContextPlaybackSession |
      kPlaybackShortcutContextVideoPlayback;
  const uint32_t videoEditingContexts =
      videoPlaybackContexts | kPlaybackShortcutContextVideoEditing;
  const uint32_t videoEditExitContexts =
      kPlaybackShortcutContextVideoEditExitConfirmation;
  ok &= expect(resolvePlaybackShortcutAction(makeKey('E'),
                                              videoPlaybackContexts)
                   .value() == PlaybackShortcutAction::ToggleVideoEditor,
               "Bare E must enter the video editor only during video playback");
  ok &= expect(!resolvePlaybackShortcutAction(
                   makeKey('E'), kPlaybackShortcutContextGlobal |
                                     kPlaybackShortcutContextShared),
               "The editor shortcut must not leak into non-video playback");
  ok &= expect(resolvePlaybackShortcutAction(makeKey(VK_ESCAPE),
                                              videoEditingContexts)
                   .value() == PlaybackShortcutAction::ExitVideoEditor,
               "Escape must close the modal editor before playback itself");
  ok &= expect(resolvePlaybackShortcutAction(makeKey('O'),
                                              videoEditingContexts)
                   .value() == PlaybackShortcutAction::SetVideoEditOut,
               "Bare O must set the edit Out point while editing");
  ok &= expect(resolvePlaybackShortcutAction(makeKey('O'),
                                              videoPlaybackContexts)
                   .value() == PlaybackShortcutAction::ToggleOptions,
               "Bare O must regain its playback-options meaning outside editing");
  ok &= expect(resolvePlaybackShortcutAction(makeKey('I'),
                                              videoEditingContexts)
                   .value() == PlaybackShortcutAction::SetVideoEditIn,
               "Bare I must set the edit In point");
  ok &= expect(resolvePlaybackShortcutAction(makeKey(VK_DELETE),
                                              videoEditingContexts)
                   .value() ==
                   PlaybackShortcutAction::RippleDeleteVideoEditSelection,
               "Delete must perform the editor's ripple removal");
  ok &= expect(resolvePlaybackShortcutAction(makeKey('T'),
                                              videoEditingContexts)
                   .value() == PlaybackShortcutAction::TrimVideoEditSelection,
               "Bare T must trim the sequence to the selected range");
  ok &= expect(!resolvePlaybackShortcutAction(makeKey('P'),
                                               videoEditingContexts),
               "Bare P must remain unbound without a real timeline player");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey('P', 0, kPlaybackShortcutCtrlMask),
                   videoEditingContexts)
                   .value() == PlaybackShortcutAction::TogglePictureInPicture,
               "Ctrl+P must retain its PiP meaning while editing");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey('E', 0, kPlaybackShortcutCtrlMask),
                   videoEditingContexts)
                   .value() == PlaybackShortcutAction::ExportVideoEdits,
               "Ctrl+E must route to the editor-owned export task");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey('E', 0, kPlaybackShortcutCtrlMask),
                   videoPlaybackContexts)
                   .value() == PlaybackShortcutAction::ExportVideoEdits,
                "Ctrl+E must keep a background edit export cancellable after closing the editor");
  ok &= expect(resolvePlaybackShortcutAction(makeKey('D'),
                                              videoEditExitContexts)
                   .value() ==
                   PlaybackShortcutAction::DiscardVideoEditsAndExit,
               "D must explicitly confirm discarding edits in the exit prompt");
  ok &= expect(resolvePlaybackShortcutAction(makeKey(VK_ESCAPE),
                                              videoEditExitContexts)
                   .value() == PlaybackShortcutAction::CancelVideoEditExit,
               "Escape must cancel the modal edit-exit prompt");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey('E', 0, kPlaybackShortcutCtrlMask),
                   videoEditExitContexts)
                   .value() == PlaybackShortcutAction::ExportVideoEdits,
               "Ctrl+E must export from the modal edit-exit prompt");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey('Z', 0, kPlaybackShortcutCtrlMask),
                   videoEditingContexts)
                   .value() == PlaybackShortcutAction::UndoVideoEdit,
               "Ctrl+Z must undo an edit decision");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey('Y', 0, kPlaybackShortcutCtrlMask),
                   videoEditingContexts)
                   .value() == PlaybackShortcutAction::RedoVideoEdit,
               "Ctrl+Y must redo an edit decision");
  ok &= expect(resolvePlaybackShortcutAction(
                   makeKey('R', 0, kPlaybackShortcutCtrlMask),
                   videoEditingContexts)
                   .value() == PlaybackShortcutAction::ResetVideoEdits,
               "Ctrl+R must reset edit decisions instead of toggling radio");
  ok &= expect(resolvePlaybackShortcutAction(makeKey(VK_OEM_COMMA, ','),
                                              videoEditingContexts)
                   .value() == PlaybackShortcutAction::PreviousFrame,
               "Frame stepping must remain available for precise In/Out marks");
  ok &= expect(playback_video_control::shouldCoalesceQueuedEvent(
                   playback_video_control::EventType::SeekRequest,
                   playback_video_control::EventType::SeekRequest),
               "Adjacent seek requests must coalesce");
  ok &= expect(!playback_video_control::shouldCoalesceQueuedEvent(
                   playback_video_control::EventType::FrameStepRequest,
                   playback_video_control::EventType::SeekRequest),
               "Seek requests must not jump over queued frame steps");
  ok &= expect(!playback_video_control::shouldCoalesceQueuedEvent(
                   playback_video_control::EventType::SeekRequest,
                   playback_video_control::EventType::FrameStepRequest),
               "Frame steps must remain explicit events after seeks");
  ok &= expect(!playback_video_control::shouldCoalesceQueuedEvent(
                   playback_video_control::EventType::FirstFramePresented,
                   playback_video_control::EventType::PauseRequest),
               "First-frame presentation events must not be folded into "
               "transport commands");

  playback_video_state_machine::PipelineSnapshot stepOpeningReady{};
  stepOpeningReady.initDone = true;
  stepOpeningReady.initOk = true;
  stepOpeningReady.videoQueueDepth = 1;
  stepOpeningReady.videoQueueEmpty = false;
  auto putStepControllerInPlaying =
      [&](playback_video_state_machine::Controller& c) {
    c.beginOpening();
    c.observe(stepOpeningReady, 1);
    c.finishPriming();
  };
  auto putStepControllerInPaused =
      [&](playback_video_state_machine::Controller& c) {
    playback_video_state_machine::PipelineSnapshot pausedOpening =
        stepOpeningReady;
    pausedOpening.audioPaused = true;
    c.beginOpening();
    c.observe(pausedOpening, 1);
  };
  auto putStepControllerInEnded =
      [&](playback_video_state_machine::Controller& c) {
    c.beginOpening();
    c.observe(stepOpeningReady, 1);
    c.finishPriming();
    playback_video_state_machine::PipelineSnapshot endedSnapshot =
        stepOpeningReady;
    endedSnapshot.decodeEnded = true;
    endedSnapshot.demuxEnded = true;
    endedSnapshot.videoQueueDepth = 0;
    endedSnapshot.videoQueueEmpty = true;
    endedSnapshot.lastPresentedSerial = endedSnapshot.currentSerial;
    c.observe(endedSnapshot, 1);
  };

  playback_video_frame_step::Request claimedStep{};
  playback_video_state_machine::Controller activeStepControl;
  putStepControllerInPlaying(activeStepControl);
  activeStepControl.resetForSerial(7);
  playback_video_state_machine::StateChange activeStep =
      activeStepControl.requestFrameStep(
          playback_video_frame_step::Direction::Previous, 7);
  ok &= expect(activeStep.changed &&
                   activeStep.previous == PlayerState::Playing &&
                   activeStep.current == PlayerState::Paused &&
                    activeStepControl.claimFrameStep(&claimedStep, 7, false) &&
                   claimedStep.direction ==
                       playback_video_frame_step::Direction::Previous,
               "Frame-step requests from active playback must pause and claim "
               "one explicit frame");

  playback_video_state_machine::Controller endedStepControl;
  putStepControllerInEnded(endedStepControl);
  endedStepControl.resetForSerial(7);
  playback_video_state_machine::StateChange endedStep =
      endedStepControl.requestFrameStep(
          playback_video_frame_step::Direction::Previous, 7);
  ok &= expect(endedStep.changed &&
                   endedStep.previous == PlayerState::Ended &&
                   endedStep.current == PlayerState::Paused &&
                    endedStepControl.claimFrameStep(&claimedStep, 7, false) &&
                   claimedStep.direction ==
                       playback_video_frame_step::Direction::Previous,
               "Frame-step requests from Ended must re-enter paused transport");

  playback_video_state_machine::Controller stepControl;
  putStepControllerInPaused(stepControl);
  stepControl.resetForSerial(7);
  stepControl.requestFrameStep(
      playback_video_frame_step::Direction::Previous, 7);
  stepControl.requestFrameStep(playback_video_frame_step::Direction::Next,
                               7);
  ok &= expect(stepControl.claimFrameStep(&claimedStep, 7, false) &&
                   claimedStep.direction ==
                       playback_video_frame_step::Direction::Previous,
               "Frame-step control must preserve explicit request order");
  playback_video_frame_step::Request oldSerialStep = claimedStep;
  stepControl.discardFrameStep(claimedStep);
  ok &= expect(stepControl.claimFrameStep(&claimedStep, 7, false) &&
                   claimedStep.direction ==
                       playback_video_frame_step::Direction::Next,
               "Frame-step control must expose the next pending request");
  stepControl.discardFrameStep(claimedStep);
  ok &= expect(!stepControl.claimFrameStep(&claimedStep, 7, false),
               "Frame-step control must clear wake state when drained");
  stepControl.requestFrameStep(
      playback_video_frame_step::Direction::Previous, 7);
  stepControl.resetForSerial(8);
  ok &= expect(!stepControl.claimFrameStep(&claimedStep, 8, false),
               "Frame-step requests must not survive serial transitions");
  stepControl.requestFrameStep(playback_video_frame_step::Direction::Next,
                               8);
  stepControl.discardFrameStep(oldSerialStep);
  ok &= expect(stepControl.claimFrameStep(&claimedStep, 8, false) &&
                   claimedStep.direction ==
                       playback_video_frame_step::Direction::Next,
               "Finishing an old serial frame-step must not pop a new serial "
               "request");
  ok &= expect(stepControl.resumePlaybackFrameSteps()
                   .requiresTimelineReanchor,
               "A frame-step claimed by the output pipeline must request a "
               "resume transition");
  ok &= expect(!stepControl.claimFrameStep(&claimedStep, 8, false),
               "Frame-step requests must not survive playback resume");
  stepControl.requestFrameStep(playback_video_frame_step::Direction::Next,
                               8);
  ok &= expect(
      !stepControl.resumePlaybackFrameSteps().requiresTimelineReanchor,
      "A frame-step not yet claimed by the output pipeline must not request "
      "a resume transition");
  stepControl.requestFrameStep(playback_video_frame_step::Direction::Previous,
                               8);
  ok &= expect(!stepControl.claimFrameStep(&claimedStep, 8, true),
               "Frame-step control must wait while a frame-step seek target is "
               "still being decoded");
  ok &= expect(stepControl.claimFrameStep(&claimedStep, 8, false) &&
                   claimedStep.direction ==
                       playback_video_frame_step::Direction::Previous,
               "Blocked frame-step requests must remain queued until the "
               "frame-step seek target is presented");
  stepControl.discardFrameStep(claimedStep);

  playback_video_state_machine::Controller stepAckControl;
  putStepControllerInPaused(stepAckControl);
  stepAckControl.resetForSerial(9);
  stepAckControl.requestFrameStep(playback_video_frame_step::Direction::Next,
                                  9);
  ok &= expect(stepAckControl.claimFrameStep(&claimedStep, 9, false),
               "Frame-step presentation test must claim a request");
  stepAckControl.publishFrameStepPresentation(claimedStep);
  playback_video_state_machine::FrameStepPresentationResult
      stepAckPresentation = stepAckControl.consumeFrameStepPresentation(
          claimedStep.serial, claimedStep.generation);
  ok &= expect(stepAckPresentation.accepted &&
                   stepAckPresentation.change.changed &&
                   stepAckPresentation.change.current ==
                       PlayerState::FrameStep,
               "Presented frame-step must be consumable while transport is "
               "still paused and must enter explicit frame-step state");
  stepAckControl.requestFrameStep(playback_video_frame_step::Direction::Next,
                                  9);
  ok &= expect(stepAckControl.claimFrameStep(&claimedStep, 9, false),
               "Frame-step resume invalidation test must claim a request");
  stepAckControl.publishFrameStepPresentation(claimedStep);
  playback_video_state_machine::FrameStepExitResult stepAckResume =
      stepAckControl.resumePlaybackFrameSteps();
  ok &= expect(stepAckResume.requiresTimelineReanchor &&
                   stepAckResume.change.changed &&
                   stepAckResume.change.current == PlayerState::Paused,
               "Resume must invalidate pending frame-step presentation acks "
               "and require a timeline re-anchor");
  ok &= expect(!stepAckControl
                    .consumeFrameStepPresentation(claimedStep.serial,
                                                  claimedStep.generation)
                    .accepted,
               "Frame-step presentation acks must not survive playback resume");

  playback_video_state_machine::Controller newestStepAckControl;
  putStepControllerInPaused(newestStepAckControl);
  newestStepAckControl.resetForSerial(10);
  newestStepAckControl.requestFrameStep(
      playback_video_frame_step::Direction::Next, 10);
  ok &= expect(newestStepAckControl.claimFrameStep(&claimedStep, 10, false),
               "Latest presentation test must claim the first request");
  playback_video_frame_step::Request firstPresentedStep = claimedStep;
  newestStepAckControl.publishFrameStepPresentation(firstPresentedStep);
  newestStepAckControl.requestFrameStep(
      playback_video_frame_step::Direction::Next, 10);
  ok &= expect(newestStepAckControl.claimFrameStep(&claimedStep, 10, false),
               "Latest presentation test must claim the second request");
  playback_video_frame_step::Request secondPresentedStep = claimedStep;
  newestStepAckControl.publishFrameStepPresentation(secondPresentedStep);
  ok &= expect(!newestStepAckControl
                    .consumeFrameStepPresentation(firstPresentedStep.serial,
                                                  firstPresentedStep.generation)
                    .accepted,
               "Older frame-step presentation acks must be superseded by the "
               "newer presented frame");
  playback_video_state_machine::FrameStepPresentationResult
      newestStepPresentation =
          newestStepAckControl.consumeFrameStepPresentation(
              secondPresentedStep.serial, secondPresentedStep.generation);
  ok &= expect(newestStepPresentation.accepted &&
                   newestStepPresentation.change.current ==
                       PlayerState::FrameStep,
               "Only the newest frame-step presentation ack should be "
               "accepted and enter frame-step state");

  playback_video_state_machine::Controller frameStepModeControl;
  putStepControllerInPaused(frameStepModeControl);
  frameStepModeControl.resetForSerial(31);
  frameStepModeControl.requestFrameStep(
      playback_video_frame_step::Direction::Next, 31);
  ok &= expect(frameStepModeControl.claimFrameStep(&claimedStep, 31, false),
               "Frame-step mode test must claim a request");
  frameStepModeControl.publishFrameStepPresentation(claimedStep);
  playback_video_state_machine::FrameStepPresentationResult
      frameStepModePresentation =
          frameStepModeControl.consumeFrameStepPresentation(
              claimedStep.serial, claimedStep.generation);
  ok &= expect(frameStepModePresentation.accepted &&
                   frameStepModePresentation.change.current ==
                       PlayerState::FrameStep,
               "Frame-step mode test must consume presentation ack");
  playback_video_state_machine::FrameStepExitResult frameStepModeResume =
      frameStepModeControl.resumePlaybackFrameSteps();
  ok &= expect(frameStepModeResume.requiresTimelineReanchor &&
                   frameStepModeResume.change.current == PlayerState::Paused,
               "Frame-step mode must stay active until playback resume");

  playback_video_state_machine::Controller stepSeekTokenControl;
  putStepControllerInPaused(stepSeekTokenControl);
  stepSeekTokenControl.resetForSerial(11);
  stepSeekTokenControl.requestFrameStep(
      playback_video_frame_step::Direction::Previous, 11);
  ok &= expect(stepSeekTokenControl.claimFrameStep(&claimedStep, 11, false),
               "Frame-step seek token test must claim a request");
  ok &= expect(stepSeekTokenControl.publishFrameStepSeek(claimedStep),
               "Frame-step seek publication must claim the active request");
  ok &= expect(!stepSeekTokenControl.claimFrameStep(&claimedStep, 11, false),
               "A pending frame-step seek must block later frame-step claims "
               "until control consumes it");
  ok &= expect(stepSeekTokenControl.consumeFrameStepSeek(
                   claimedStep.serial, claimedStep.generation),
                "Frame-step seek token must be consumable while still paused");
  stepSeekTokenControl.resetForFrameStepSeekSerial(12);
  ok &= expect(stepSeekTokenControl.publishFrameStepSeekPresentation(
                   12, claimedStep.generation),
               "Frame-step seek must transfer presentation acknowledgement to "
               "the redecode serial");
  playback_video_state_machine::FrameStepPresentationResult
      stepSeekPresentation = stepSeekTokenControl.consumeFrameStepPresentation(
          12, claimedStep.generation);
  ok &= expect(stepSeekPresentation.accepted &&
                   stepSeekPresentation.change.current ==
                       PlayerState::FrameStep,
               "Frame-step seek presentation must be acknowledged on the "
               "redecode serial");
  stepSeekTokenControl.requestFrameStep(
      playback_video_frame_step::Direction::Previous, 12);
  ok &= expect(stepSeekTokenControl.claimFrameStep(&claimedStep, 12, false),
               "Frame-step seek resume invalidation test must claim a request");
  ok &= expect(stepSeekTokenControl.publishFrameStepSeek(claimedStep),
               "Frame-step seek resume invalidation test must publish token");
  ok &= expect(stepSeekTokenControl.resumePlaybackFrameSteps()
                   .requiresTimelineReanchor,
               "Resume must invalidate pending frame-step seek tokens and "
               "retain the timeline re-anchor requirement");
  ok &= expect(!stepSeekTokenControl.consumeFrameStepSeek(
                   claimedStep.serial, claimedStep.generation),
                "Frame-step seek tokens must not survive playback resume");

  playback_video_state_machine::Controller abortedStepSeekControl;
  putStepControllerInPaused(abortedStepSeekControl);
  abortedStepSeekControl.resetForSerial(20);
  abortedStepSeekControl.requestFrameStep(
      playback_video_frame_step::Direction::Previous, 20);
  ok &= expect(abortedStepSeekControl.claimFrameStep(&claimedStep, 20, false) &&
                   abortedStepSeekControl.publishFrameStepSeek(claimedStep) &&
                   abortedStepSeekControl.consumeFrameStepSeek(
                       claimedStep.serial, claimedStep.generation),
               "Aborted frame-step seek test must hand off one seek request");
  abortedStepSeekControl.resetForFrameStepSeekSerial(21);
  abortedStepSeekControl.beginSeeking();
  ok &= expect(abortedStepSeekControl.publishFrameStepSeekPresentation(
                   21, claimedStep.generation),
               "Aborted frame-step seek test must publish the redecode "
               "presentation token");
  abortedStepSeekControl.cancelFrameStepSeek();
  playback_video_state_machine::FrameStepExitResult abortedStepSeekResume =
      abortedStepSeekControl.resumePlaybackFrameSteps();
  ok &= expect(abortedStepSeekResume.requiresTimelineReanchor &&
                   !abortedStepSeekResume.change.changed &&
                   abortedStepSeekResume.change.current == PlayerState::Seeking,
               "An aborted frame-step seek must preserve the resume "
               "re-anchor requirement after its presentation token is gone");

  playback_video_state_machine::Controller replacedStepSeekControl;
  putStepControllerInPaused(replacedStepSeekControl);
  replacedStepSeekControl.resetForFrameStepSeekSerial(30);
  replacedStepSeekControl.resetForSerial(31);
  ok &= expect(!replacedStepSeekControl.resumePlaybackFrameSteps()
                    .requiresTimelineReanchor,
               "A normal serial transition must replace a pending frame-step "
               "resume re-anchor");

  playback_video_state_machine::Controller replayResumeControl;
  putStepControllerInPaused(replayResumeControl);
  playback_video_state_machine::PipelineSnapshot replayPendingSnapshot =
      stepOpeningReady;
  replayPendingSnapshot.audioPaused = false;
  replayPendingSnapshot.decodeEnded = true;
  replayPendingSnapshot.demuxEnded = true;
  replayPendingSnapshot.videoQueueDepth = 0;
  replayPendingSnapshot.videoQueueEmpty = true;
  replayPendingSnapshot.lastPresentedSerial =
      replayPendingSnapshot.currentSerial;
  replayPendingSnapshot.videoReplayPending = true;
  playback_video_state_machine::Evaluation replayResume =
      replayResumeControl.observe(replayPendingSnapshot, 1);
  ok &= expect(replayResume.change.changed &&
                   replayResume.change.current == PlayerState::Playing,
               "Resume with history replay pending must not jump straight to "
               "Ended");
  replayPendingSnapshot.videoReplayPending = false;
  playback_video_state_machine::Evaluation replayDrained =
      replayResumeControl.observe(replayPendingSnapshot, 1);
  ok &= expect(replayDrained.change.changed &&
                   replayDrained.change.current == PlayerState::Ended,
               "Playback may enter Ended after history replay is drained");
  playback_video_state_machine::Controller replayResetControl;
  putStepControllerInPaused(replayResetControl);
  replayPendingSnapshot.videoReplayPending = true;
  playback_video_state_machine::Evaluation replayBeforeSerialReset =
      replayResetControl.observe(replayPendingSnapshot, 1);
  ok &= expect(replayBeforeSerialReset.change.changed &&
                   replayBeforeSerialReset.change.current ==
                       PlayerState::Playing,
               "Pipeline replay state must gate Ended before serial reset");
  replayPendingSnapshot.videoReplayPending = false;
  replayResetControl.resetForSerial(8);
  playback_video_state_machine::Evaluation replayAfterSerialReset =
      replayResetControl.observe(replayPendingSnapshot, 1);
  ok &= expect(replayAfterSerialReset.change.changed &&
                   replayAfterSerialReset.change.current == PlayerState::Ended,
               "Cleared pipeline replay state must allow Ended after serial "
               "reset");

  playback_video_frame_cursor::Controller frameCursor;
  frameCursor.resetForSerial(1);
  auto appendCursorFrame = [&](int64_t ptsUs) {
    static uint64_t displayIndex = 1;
    QueuedFrame item{};
    item.ptsUs = ptsUs;
    item.durationUs = 40000;
    item.serial = 1;
    item.info.timestamp100ns = ptsUs * 10;
    item.displayIndex = displayIndex;
    ++displayIndex;
    VideoFrame frame{};
    frame.timestamp100ns = ptsUs * 10;
    frameCursor.noteDecoded(item);
    frameCursor.appendPresented(item, frame);
  };
  appendCursorFrame(1000);
  appendCursorFrame(2000);
  appendCursorFrame(3000);
  ok &= expect(frameCursor.atNewestFrame(),
               "Frame cursor must track the newest presented frame");
  ok &= expect(!frameCursor.replayPendingForSerial(1),
               "Frame cursor must publish no replay work at the newest frame");
  const playback_video_frame_cursor::PresentedFrame* previousFrame =
      frameCursor.step(playback_video_frame_step::Direction::Previous);
  ok &= expect(previousFrame && previousFrame->ptsUs == 2000,
               "Previous-frame must move to the exact previous frame entry");
  ok &= expect(previousFrame && previousFrame->displayIndex == 2,
               "Previous-frame must move by display order, not timestamp math");
  ok &= expect(frameCursor.replayPendingForSerial(1),
               "Previous-frame must publish replay work while off newest");
  ok &= expect(!frameCursor.replayPendingForSerial(2),
               "Frame cursor replay publication must not leak across serials");
  const playback_video_frame_cursor::PresentedFrame* nextFrame =
      frameCursor.step(playback_video_frame_step::Direction::Next);
  ok &= expect(nextFrame && nextFrame->ptsUs == 3000,
               "Next-frame must return to the exact next frame entry");
  ok &= expect(nextFrame && nextFrame->displayIndex == 3,
               "Next-frame must return by display order, not timestamp math");
  ok &= expect(!frameCursor.replayPendingForSerial(1),
               "Next-frame back to newest must clear replay publication");
  for (int i = 0; i < 8; ++i) {
    previousFrame =
        frameCursor.step(playback_video_frame_step::Direction::Previous);
    ok &= expect(previousFrame && previousFrame->ptsUs == 2000,
                 "Repeated previous/next toggles must not drift backward");
    nextFrame = frameCursor.step(playback_video_frame_step::Direction::Next);
    ok &= expect(nextFrame && nextFrame->ptsUs == 3000,
                 "Repeated previous/next toggles must not drift forward");
  }
  previousFrame =
      frameCursor.step(playback_video_frame_step::Direction::Previous);
  ok &= expect(previousFrame && previousFrame->ptsUs == 2000,
               "Frame cursor test setup must move off the newest frame");
  ok &= expect(!frameCursor.atNewestFrame(),
               "Frame cursor must report when history replay is pending");
  ok &= expect(frameCursor.replayPendingForSerial(1),
               "Frame cursor replay publication must match history replay");
  appendCursorFrame(2500);
  ok &= expect(frameCursor.atNewestFrame(),
               "Appending a later decoded frame while replaying history must "
               "move the cursor to the newest decoded frame");
  ok &= expect(frameCursor.atNewestFrame(),
               "Appending a new presented frame must clear replay state");
  ok &= expect(!frameCursor.replayPendingForSerial(1),
               "Appending a new presented frame must clear replay publication");
  playback_video_frame_cursor::StepTarget futurePrevious =
      frameCursor.target(playback_video_frame_step::Direction::Previous);
  ok &= expect(futurePrevious.kind ==
                   playback_video_frame_cursor::StepTargetKind::Seek,
               "Previous-frame must not rewrite decode-order history when a "
               "newer frame arrives during history replay");
  ok &= expect(futurePrevious.seek.target.logicalIndex == 3 &&
                   futurePrevious.seek.target.ptsUs == 3000,
               "Previous-frame must return to the original logical neighbor "
               "instead of manufacturing a replacement branch");
  ok &= expect(!frameCursor.replayPendingForSerial(1),
               "Newest-frame boundary must keep replay publication clear");

  playback_video_frame_cursor::Controller duplicatePtsCursor;
  duplicatePtsCursor.resetForSerial(1);
  auto appendDuplicatePtsFrame = [&](uint64_t displayIndex) {
    QueuedFrame item{};
    item.ptsUs = 10000;
    item.durationUs = 40000;
    item.serial = 1;
    item.displayIndex = displayIndex;
    VideoFrame frame{};
    frame.timestamp100ns = 100000;
    duplicatePtsCursor.noteDecoded(item);
    duplicatePtsCursor.appendPresented(item, frame);
  };
  appendDuplicatePtsFrame(1);
  appendDuplicatePtsFrame(2);
  appendDuplicatePtsFrame(3);
  const playback_video_frame_cursor::PresentedFrame* duplicatePrevious =
      duplicatePtsCursor.step(playback_video_frame_step::Direction::Previous);
  ok &= expect(duplicatePrevious && duplicatePrevious->displayIndex == 2,
               "Previous-frame must not drift when adjacent frames share a PTS");
  const playback_video_frame_cursor::PresentedFrame* duplicateNext =
      duplicatePtsCursor.step(playback_video_frame_step::Direction::Next);
  ok &= expect(duplicateNext && duplicateNext->displayIndex == 3,
               "Next-frame must not drift when adjacent frames share a PTS");

  playback_video_frame_cursor::Controller decodedGapCursor;
  decodedGapCursor.resetForSerial(1);
  QueuedFrame decodedGapFirst{};
  decodedGapFirst.ptsUs = 10000;
  decodedGapFirst.durationUs = 10000;
  decodedGapFirst.serial = 1;
  decodedGapFirst.displayIndex = 1;
  VideoFrame decodedGapFirstFrame{};
  decodedGapFirstFrame.timestamp100ns = decodedGapFirst.ptsUs * 10;
  decodedGapCursor.noteDecoded(decodedGapFirst);
  decodedGapCursor.appendPresented(decodedGapFirst, decodedGapFirstFrame);
  QueuedFrame decodedGapDropped{};
  decodedGapDropped.ptsUs = 20000;
  decodedGapDropped.durationUs = 10000;
  decodedGapDropped.serial = 1;
  decodedGapDropped.displayIndex = 2;
  decodedGapCursor.noteDecoded(decodedGapDropped);
  playback_video_frame_cursor::StepTarget decodedGapNext =
      decodedGapCursor.target(playback_video_frame_step::Direction::Next);
  ok &= expect(decodedGapNext.kind ==
                   playback_video_frame_cursor::StepTargetKind::Seek,
               "Next-frame must seek/redecode to a decoded frame that was not "
               "presented as a retained surface");
  ok &= expect(decodedGapNext.seek.target.logicalIndex == 2 &&
                   decodedGapNext.seek.target.ptsUs == 20000,
               "Decoded-but-unpresented frames must keep exact logical frame "
               "identity for stepping");

  QueuedFrame decodedGapThird{};
  decodedGapThird.ptsUs = 30000;
  decodedGapThird.durationUs = 10000;
  decodedGapThird.serial = 1;
  decodedGapThird.displayIndex = 3;
  VideoFrame decodedGapThirdFrame{};
  decodedGapThirdFrame.timestamp100ns = decodedGapThird.ptsUs * 10;
  decodedGapCursor.noteDecoded(decodedGapThird);
  decodedGapCursor.appendPresented(decodedGapThird, decodedGapThirdFrame);
  playback_video_frame_cursor::StepTarget decodedGapPrevious =
      decodedGapCursor.target(playback_video_frame_step::Direction::Previous);
  ok &= expect(decodedGapPrevious.kind ==
                   playback_video_frame_cursor::StepTargetKind::Seek,
               "Previous-frame must not jump over decoded-but-unpresented "
               "frames by reusing an older retained surface");
  ok &= expect(decodedGapPrevious.seek.target.logicalIndex == 2 &&
                   decodedGapPrevious.seek.target.ptsUs == 20000,
               "Previous-frame seek/redecode must target the skipped logical "
               "neighbor without drift");

  static_assert(playback_video_frame_cursor::kRetainedFrameCount == 2,
                "Frame cursor must keep decoder surfaces short-lived");
  playback_video_frame_cursor::Controller boundedCursor;
  boundedCursor.resetForSerial(1);
  for (uint64_t displayIndex = 1; displayIndex <= 6; ++displayIndex) {
    QueuedFrame item{};
    item.ptsUs = static_cast<int64_t>(displayIndex) * 10000;
    item.durationUs = 10000;
    item.serial = 1;
    item.displayIndex = displayIndex;
    VideoFrame frame{};
    frame.timestamp100ns = item.ptsUs * 10;
    boundedCursor.noteDecoded(item);
    boundedCursor.appendPresented(item, frame);
  }
  previousFrame =
      boundedCursor.step(playback_video_frame_step::Direction::Previous);
  ok &= expect(previousFrame && previousFrame->displayIndex == 5,
               "Frame cursor must keep the adjacent display frame as a "
               "retained surface");
  playback_video_frame_cursor::StepTarget previousSeekTarget =
      boundedCursor.target(playback_video_frame_step::Direction::Previous);
  ok &= expect(previousSeekTarget.kind ==
                   playback_video_frame_cursor::StepTargetKind::Seek,
               "Frame cursor must return a seek target when the previous "
               "surface is no longer retained");
  ok &= expect(previousSeekTarget.seek.target.logicalIndex == 4 &&
                   previousSeekTarget.seek.target.ptsUs == 40000,
               "Previous-frame seek target must preserve exact logical frame "
               "identity");
  ok &= expect(previousSeekTarget.seek.seekTargetUs() == 40000,
               "Previous-frame seek/redecode must keep the presentation "
               "target on the exact requested frame");
  ok &= expect(previousSeekTarget.seek.anchor.logicalIndex == 3 &&
                    previousSeekTarget.seek.anchor.ptsUs == 30000,
                "Previous-frame seek/redecode must anchor before the target "
                "frame instead of at the target boundary");
  ok &= expect(previousSeekTarget.seek.demuxTargetUs() == 0,
               "Previous-frame seek/redecode must seek with decode lead-in "
               "before the anchor timestamp");
  previousSeekTarget.seek.generation = 99;
  boundedCursor.resetForSerial(2, &previousSeekTarget.seek);
  ok &= expect(boundedCursor.frameStepSeekPendingForSerial(2),
               "Frame-step seek/redecode must block new frame-step claims "
               "until the target frame is presented");
  QueuedFrame redecodeAnchor{};
  redecodeAnchor.ptsUs = previousSeekTarget.seek.anchor.ptsUs;
  redecodeAnchor.durationUs = previousSeekTarget.seek.anchor.durationUs;
  redecodeAnchor.serial = 2;
  redecodeAnchor.displayIndex = 1;
  playback_video_frame_cursor::PendingSeekFrameDecision anchorDecision =
      boundedCursor.inspectPendingSeekFrame(redecodeAnchor);
  ok &= expect(anchorDecision.action ==
                   playback_video_frame_cursor::PendingSeekFrameAction::
                       DropPreroll,
               "Previous-frame seek/redecode must discard the anchor frame "
               "before presenting the target");
  QueuedFrame redecodePrevious{};
  redecodePrevious.ptsUs = previousSeekTarget.seek.target.ptsUs;
  redecodePrevious.durationUs = previousSeekTarget.seek.target.durationUs;
  redecodePrevious.serial = 2;
  redecodePrevious.displayIndex = 2;
  playback_video_frame_cursor::PendingSeekFrameDecision previousDecision =
      boundedCursor.inspectPendingSeekFrame(redecodePrevious);
  ok &= expect(previousDecision.action ==
                   playback_video_frame_cursor::PendingSeekFrameAction::
                       PresentTarget,
               "Previous-frame seek/redecode must stop preroll discard at the "
               "target frame");
  std::optional<playback_video_frame_step::Request> redecodeStepRequest =
      boundedCursor.pendingFrameStepRequestForSerial(2);
  ok &= expect(redecodeStepRequest &&
                   redecodeStepRequest->generation == 99 &&
                   redecodeStepRequest->serial == 2,
               "Frame-step seek/redecode must preserve the original request "
               "identity for the presentation acknowledgement");
  VideoFrame redecodePreviousFrame{};
  redecodePreviousFrame.timestamp100ns = redecodePrevious.ptsUs * 10;
  boundedCursor.appendPresented(redecodePrevious, redecodePreviousFrame);
  ok &= expect(!boundedCursor.frameStepSeekPendingForSerial(2),
               "Frame-step seek/redecode must clear its pending target only "
               "after presentation has been recorded");
  playback_video_frame_cursor::StepTarget nextSeekTarget =
      boundedCursor.target(playback_video_frame_step::Direction::Next);
  ok &= expect(nextSeekTarget.kind ==
                   playback_video_frame_cursor::StepTargetKind::Seek,
               "Next-frame must use seek/redecode when the logical next frame "
               "is known but no retained surface exists");
  ok &= expect(nextSeekTarget.seek.target.logicalIndex == 5 &&
                   nextSeekTarget.seek.target.ptsUs == 50000,
               "Next-frame seek target must return to the exact original "
               "neighbor without drift");
  ok &= expect(nextSeekTarget.seek.seekTargetUs() == 50000,
               "Next-frame seek/redecode must keep the presentation target on "
               "the exact requested frame");
  ok &= expect(nextSeekTarget.seek.anchor.logicalIndex == 4 &&
                    nextSeekTarget.seek.anchor.ptsUs == 40000,
                "Next-frame seek/redecode must anchor at the current logical "
                "frame before decoding forward");
  ok &= expect(nextSeekTarget.seek.demuxTargetUs() == 0,
               "Next-frame seek/redecode must seek with decode lead-in before "
               "the anchor timestamp");

  playback_video_frame_cursor::Controller seekBoundaryCursor;
  seekBoundaryCursor.resetForSerial(1);
  QueuedFrame seekBoundaryCurrent{};
  seekBoundaryCurrent.ptsUs = 100000;
  seekBoundaryCurrent.durationUs = 40000;
  seekBoundaryCurrent.serial = 1;
  seekBoundaryCurrent.displayIndex = 1;
  VideoFrame seekBoundaryCurrentFrame{};
  seekBoundaryCurrentFrame.timestamp100ns = seekBoundaryCurrent.ptsUs * 10;
  seekBoundaryCursor.noteDecoded(seekBoundaryCurrent);
  seekBoundaryCursor.appendPresented(seekBoundaryCurrent,
                                     seekBoundaryCurrentFrame);
  playback_video_frame_cursor::StepTarget missingPreviousAfterSeek =
      seekBoundaryCursor.target(playback_video_frame_step::Direction::Previous);
  ok &= expect(missingPreviousAfterSeek.kind ==
                   playback_video_frame_cursor::StepTargetKind::Seek,
               "Previous-frame after a normal seek must start redecode even "
               "when no earlier frame record exists in the new serial");
  ok &= expect(missingPreviousAfterSeek.seek.mode ==
                   playback_video_frame_step_seek::PlanMode::
                       PreviousBeforeTarget,
               "Unknown previous-frame stepping must use boundary discovery, "
               "not timestamp math");
  ok &= expect(missingPreviousAfterSeek.seek.seekTargetUs() == 100000 &&
                   missingPreviousAfterSeek.seek.demuxTargetUs() == 0 &&
                   missingPreviousAfterSeek.seek.demuxWindowEndUs() == 100000 &&
                   missingPreviousAfterSeek.seek.decoderPrerollTargetUs() == 0,
               "Previous-frame boundary discovery must seek with enough "
               "decode lead-in to find the frame before the current boundary");
  playback_video_frame_step_seek::Plan laterBoundaryPlan =
      missingPreviousAfterSeek.seek;
  laterBoundaryPlan.target.ptsUs = 2000000;
  laterBoundaryPlan.anchor = laterBoundaryPlan.target;
  ok &= expect(
      laterBoundaryPlan.demuxTargetUs() ==
              2000000 -
                  playback_video_frame_step_seek::
                      kFrameStepDecodeLeadInUs &&
          laterBoundaryPlan.demuxWindowEndUs() == 2000000,
      "Previous-frame discovery must use a stable decode lead-in instead of "
      "landing directly on the boundary");
  AVRational thirtyFpsTimeBase{1, 30};
  int64_t boundaryUs =
      av_rescale_q(3, thirtyFpsTimeBase, AVRational{1, AV_TIME_BASE});
  int64_t roundedNearestTimestamp =
      av_rescale_q(boundaryUs - 1, AVRational{1, AV_TIME_BASE},
                   thirtyFpsTimeBase);
  int64_t floorTimestamp =
      playback_video_timeline::videoStreamTimestampFloorForUs(
          boundaryUs - 1, thirtyFpsTimeBase);
  ok &= expect(boundaryUs == 100000 && roundedNearestTimestamp == 3,
               "Previous-frame discovery regression setup must reproduce "
               "normal rescale rounding back onto the boundary frame");
  ok &= expect(floorTimestamp == 2,
               "Previous-frame discovery demux seek must floor stream "
               "timestamps so target-1us cannot land on the boundary frame");
  missingPreviousAfterSeek.seek.generation = 101;
  seekBoundaryCursor.resetForSerial(2, &missingPreviousAfterSeek.seek);
  QueuedFrame earlierDiscoveryCandidate{};
  earlierDiscoveryCandidate.ptsUs = 20000;
  earlierDiscoveryCandidate.durationUs = 40000;
  earlierDiscoveryCandidate.serial = 2;
  earlierDiscoveryCandidate.displayIndex = 1;
  VideoFrame earlierDiscoveryFrame{};
  earlierDiscoveryFrame.timestamp100ns = earlierDiscoveryCandidate.ptsUs * 10;
  playback_video_frame_cursor::PendingSeekFrameDecision firstCandidateDecision =
      seekBoundaryCursor.inspectPendingSeekFrame(earlierDiscoveryCandidate,
                                                 &earlierDiscoveryFrame);
  ok &= expect(firstCandidateDecision.action ==
                   playback_video_frame_cursor::PendingSeekFrameAction::
                       SaveBackstepCandidate,
               "Boundary discovery must save decoded frames before the current "
               "boundary instead of waiting for queue lookahead");
  QueuedFrame discoveredPrevious{};
  discoveredPrevious.ptsUs = 60000;
  discoveredPrevious.durationUs = 40000;
  discoveredPrevious.serial = 2;
  discoveredPrevious.displayIndex = 2;
  VideoFrame discoveredPreviousFrame{};
  discoveredPreviousFrame.timestamp100ns = discoveredPrevious.ptsUs * 10;
  playback_video_frame_cursor::PendingSeekFrameDecision latestCandidateDecision =
      seekBoundaryCursor.inspectPendingSeekFrame(discoveredPrevious,
                                                 &discoveredPreviousFrame);
  ok &= expect(latestCandidateDecision.action ==
                   playback_video_frame_cursor::PendingSeekFrameAction::
                       SaveBackstepCandidate &&
                   latestCandidateDecision.target.ptsUs ==
                       discoveredPrevious.ptsUs,
               "Boundary discovery must replace the saved candidate until the "
               "current boundary is reached");
  QueuedFrame rediscoveredCurrent{};
  rediscoveredCurrent.ptsUs = seekBoundaryCurrent.ptsUs;
  rediscoveredCurrent.durationUs = seekBoundaryCurrent.durationUs;
  rediscoveredCurrent.serial = 2;
  rediscoveredCurrent.displayIndex = 3;
  VideoFrame rediscoveredCurrentFrame{};
  rediscoveredCurrentFrame.timestamp100ns = rediscoveredCurrent.ptsUs * 10;
  playback_video_frame_cursor::PendingSeekFrameDecision discoveredDecision =
      seekBoundaryCursor.inspectPendingSeekFrame(rediscoveredCurrent,
                                                 &rediscoveredCurrentFrame);
  ok &= expect(discoveredDecision.action ==
                   playback_video_frame_cursor::PendingSeekFrameAction::
                       PresentBackstepCandidate &&
                   discoveredDecision.target.ptsUs == discoveredPrevious.ptsUs,
               "Boundary discovery must present the latest saved decoded frame "
               "before the current seek boundary");
  seekBoundaryCursor.appendPresented(discoveredPrevious,
                                     discoveredPreviousFrame);
  playback_video_frame_cursor::StepTarget seekBackToCurrent =
      seekBoundaryCursor.target(playback_video_frame_step::Direction::Next);
  ok &= expect(seekBackToCurrent.kind ==
                   playback_video_frame_cursor::StepTargetKind::Seek &&
                   seekBackToCurrent.seek.mode ==
                       playback_video_frame_step_seek::PlanMode::ExactFrame &&
                   seekBackToCurrent.seek.target.ptsUs ==
                       seekBoundaryCurrent.ptsUs,
               "After discovering an unbuffered previous frame, next-frame "
               "must return to the original seek frame through exact redecode");

  playback_video_frame_cursor::Controller missedBoundaryCursor;
  missedBoundaryCursor.resetForSerial(1);
  missedBoundaryCursor.noteDecoded(seekBoundaryCurrent);
  missedBoundaryCursor.appendPresented(seekBoundaryCurrent,
                                       seekBoundaryCurrentFrame);
  playback_video_frame_cursor::StepTarget missedBoundaryPrevious =
      missedBoundaryCursor.target(playback_video_frame_step::Direction::Previous);
  missedBoundaryPrevious.seek.generation = 102;
  missedBoundaryCursor.resetForSerial(2, &missedBoundaryPrevious.seek);
  playback_video_frame_cursor::PendingSeekFrameDecision missedBoundaryDecision =
      missedBoundaryCursor.inspectPendingSeekFrame(rediscoveredCurrent);
  ok &= expect(missedBoundaryDecision.action ==
                   playback_video_frame_cursor::PendingSeekFrameAction::
                       CancelWithoutDropping,
               "A previous-frame discovery miss must preserve the queued "
               "boundary frame instead of dropping the current image");

  playback_video_frame_step_seek::Plan firstFrameSeekPlan;
  firstFrameSeekPlan.direction = playback_video_frame_step::Direction::Previous;
  firstFrameSeekPlan.generation = 100;
  firstFrameSeekPlan.target.ptsUs = 10000;
  firstFrameSeekPlan.target.durationUs = 10000;
  firstFrameSeekPlan.target.logicalIndex = 1;
  firstFrameSeekPlan.anchor = firstFrameSeekPlan.target;
  boundedCursor.resetForSerial(4, &firstFrameSeekPlan);
  QueuedFrame unrecoverableMissedTarget{};
  unrecoverableMissedTarget.ptsUs = 20000;
  unrecoverableMissedTarget.durationUs = 10000;
  unrecoverableMissedTarget.serial = 4;
  unrecoverableMissedTarget.displayIndex = 1;
  playback_video_frame_cursor::PendingSeekFrameDecision unrecoverableMiss =
      boundedCursor.inspectPendingSeekFrame(unrecoverableMissedTarget);
  ok &= expect(unrecoverableMiss.action ==
                   playback_video_frame_cursor::PendingSeekFrameAction::
                       MissedTarget,
               "Frame-step seek/redecode must report a missed target when "
               "there is no earlier anchor left to try");
  ok &= expect(boundedCursor.cancelPendingFrameStepSeekForSerial(4) &&
                   !boundedCursor.frameStepSeekPendingForSerial(4),
               "Unrecoverable frame-step seek misses must be cancellable "
               "without presenting a wrong frame");

  playback_video_frame_cursor::Controller duplicateSeekCursor;
  duplicateSeekCursor.resetForSerial(1);
  for (uint64_t displayIndex = 1; displayIndex <= 4; ++displayIndex) {
    QueuedFrame item{};
    item.ptsUs = displayIndex <= 3 ? 10000 : 20000;
    item.durationUs = 10000;
    item.serial = 1;
    item.displayIndex = displayIndex;
    VideoFrame frame{};
    frame.timestamp100ns = item.ptsUs * 10;
    duplicateSeekCursor.noteDecoded(item);
    duplicateSeekCursor.appendPresented(item, frame);
  }
  duplicateSeekCursor.step(playback_video_frame_step::Direction::Previous);
  playback_video_frame_cursor::StepTarget duplicateSeekTarget =
      duplicateSeekCursor.target(playback_video_frame_step::Direction::Previous);
  ok &= expect(duplicateSeekTarget.kind ==
                   playback_video_frame_cursor::StepTargetKind::Seek,
               "Duplicate-PTS frame stepping must still produce a decode "
               "target when the surface is gone");
  ok &= expect(duplicateSeekTarget.seek.anchor.logicalIndex == 1,
               "Duplicate-PTS seek/redecode must anchor at the first matching "
               "PTS so the decoder can count to the requested logical frame");
  duplicateSeekCursor.resetForSerial(2, &duplicateSeekTarget.seek);
  QueuedFrame duplicatePreroll{};
  duplicatePreroll.ptsUs = duplicateSeekTarget.seek.anchor.ptsUs;
  duplicatePreroll.durationUs = duplicateSeekTarget.seek.anchor.durationUs;
  duplicatePreroll.serial = 2;
  duplicatePreroll.displayIndex = 1;
  playback_video_frame_cursor::PendingSeekFrameDecision duplicatePrerollDecision =
      duplicateSeekCursor.inspectPendingSeekFrame(duplicatePreroll);
  ok &= expect(duplicatePrerollDecision.action ==
                   playback_video_frame_cursor::PendingSeekFrameAction::
                       DropPreroll,
               "Frame-step seek/redecode must drop duplicate-PTS preroll "
               "frames before the target logical frame");
  QueuedFrame duplicateTarget{};
  duplicateTarget.ptsUs = duplicateSeekTarget.seek.target.ptsUs;
  duplicateTarget.durationUs = duplicateSeekTarget.seek.target.durationUs;
  duplicateTarget.serial = 2;
  duplicateTarget.displayIndex = 2;
  playback_video_frame_cursor::PendingSeekFrameDecision duplicateTargetDecision =
      duplicateSeekCursor.inspectPendingSeekFrame(duplicateTarget);
  ok &= expect(duplicateTargetDecision.action ==
                   playback_video_frame_cursor::PendingSeekFrameAction::
                       PresentTarget,
               "Frame-step seek/redecode must stop dropping at the target "
               "logical frame");

  static_assert(
      playback_video_frame_step_prefetch::kWindowDurationUs == 1000000,
      "Frame-step prefetch must keep a one-second active horizon");
  static_assert(
      playback_video_frame_step_prefetch::kDirectionChangeReserveFrameCount >=
          5,
      "Frame-step cache must keep a useful inverse-direction reserve");
  ok &= expect(
      playback_video_frame_step_prefetch::refillNeeded(
          playback_video_frame_step_prefetch::kRefillLeadFrameCount,
          playback_video_frame_step_prefetch::kRefillLeadDurationUs + 1) &&
          playback_video_frame_step_prefetch::refillNeeded(
              playback_video_frame_step_prefetch::kRefillLeadFrameCount + 1,
              playback_video_frame_step_prefetch::kRefillLeadDurationUs) &&
          !playback_video_frame_step_prefetch::refillNeeded(
              playback_video_frame_step_prefetch::kRefillLeadFrameCount + 1,
              playback_video_frame_step_prefetch::kRefillLeadDurationUs + 1),
      "Frame-step refill must start before either the frame or time lead is "
      "exhausted");

  auto makeSourceFrame = [](int64_t ptsUs, int64_t durationUs,
                            int64_t sourcePtsTicks, int64_t sourceDtsTicks,
                            int width = 0) {
    auto frame =
        std::make_shared<playback_video_frame_step_prefetch::SourceFrame>();
    frame->ptsUs = ptsUs;
    frame->sourcePtsUs = ptsUs;
    frame->durationUs = durationUs;
    frame->sourceDurationUs = durationUs;
    frame->info.sourcePtsTicks = sourcePtsTicks;
    frame->info.sourceDtsTicks = sourceDtsTicks;
    frame->info.timestamp100ns = ptsUs * 10;
    frame->info.duration100ns = durationUs * 10;
    frame->identity = playback_video_frame_step_prefetch::identityFrom(
        frame->info, frame->sourcePtsUs, frame->durationUs);
    frame->frame.timestamp100ns = frame->sourcePtsUs * 10;
    frame->frame.duration100ns = frame->durationUs * 10;
    frame->frame.width = width;
    return frame;
  };
  auto makeTickFrame = [&](int64_t tick, int64_t dts = -1, int width = 0) {
    return makeSourceFrame(tick * 10000, 10000, tick, dts >= 0 ? dts : tick,
                           width);
  };
  auto makeSequenceFrame = [&](int64_t presentationPtsUs,
                               int64_t sourcePtsUs, int64_t sourceTick) {
    auto frame = makeSourceFrame(presentationPtsUs, 10000, sourceTick,
                                 sourceTick);
    frame->sourcePtsUs = sourcePtsUs;
    frame->info.timestamp100ns = sourcePtsUs * 10;
    frame->identity = playback_video_frame_step_prefetch::identityFrom(
        frame->info, sourcePtsUs, frame->sourceDurationUs);
    frame->frame.timestamp100ns = sourcePtsUs * 10;
    return frame;
  };

  playback_video_frame_step_prefetch::SourceFrameCache sourceCache;
  std::vector<
      std::shared_ptr<const playback_video_frame_step_prefetch::SourceFrame>>
      prefixRun;
  for (int64_t tick = 0; tick < 10; ++tick) {
    prefixRun.push_back(makeTickFrame(tick));
  }
  auto join10 = makeTickFrame(10);
  ok &=
      expect(sourceCache.commitDecodedRun(
                 playback_video_frame_step::Direction::Previous,
                 join10->identity, join10, prefixRun) &&
                 sourceCache.size() == 11,
             "A decoded prefix must commit as one exact frame-identity chain");
  playback_video_frame_step_prefetch::FrameWindow sourcePrefixWindow =
      sourceCache.windowAround(join10->identity, 1000000, 0, 240);
  ok &= expect(sourcePrefixWindow.valid() &&
                   sourcePrefixWindow.anchorIndex == 10 &&
                   sourcePrefixWindow.frames.front()->sourcePtsUs == 0,
               "Cache traversal must derive look-behind from adjacency, not "
               "a claimed timestamp coverage span");

  std::vector<
      std::shared_ptr<const playback_video_frame_step_prefetch::SourceFrame>>
      suffixRun{makeSourceFrame(110000, 10000, 11, 101),
                makeSourceFrame(110000, 10000, 11, 102), makeTickFrame(12)};
  ok &= expect(
      sourceCache.commitDecodedRun(playback_video_frame_step::Direction::Next,
                                   join10->identity, nullptr, suffixRun),
      "A decoded suffix must extend the same cache incrementally");
  const size_t connectedSize = sourceCache.size();
  std::vector<
      std::shared_ptr<const playback_video_frame_step_prefetch::SourceFrame>>
      conflictingRun{makeTickFrame(99)};
  ok &= expect(!sourceCache.commitDecodedRun(
                   playback_video_frame_step::Direction::Next, join10->identity,
                   nullptr, conflictingRun) &&
                   sourceCache.size() == connectedSize &&
                   !sourceCache.find(conflictingRun.front()->identity),
               "A conflicting edge must reject the complete cache transaction");
  playback_video_frame_step_prefetch::SourceFrameCache reversedCache;
  std::vector<
      std::shared_ptr<const playback_video_frame_step_prefetch::SourceFrame>>
      reversedRun{makeSourceFrame(90000, 10000, 11, 11)};
  ok &=
      expect(!reversedCache.commitDecodedRun(
                 playback_video_frame_step::Direction::Next, join10->identity,
                 join10, reversedRun) &&
                 reversedCache.empty(),
             "A cache transaction must not manufacture forward adjacency to an "
             "earlier timestamp");
  playback_video_frame_step_prefetch::SourceFrameCache sequenceCache;
  auto secondClipJoin = makeSequenceFrame(1000000, 4000000, 400);
  std::vector<
      std::shared_ptr<const playback_video_frame_step_prefetch::SourceFrame>>
      firstClipTail{makeSequenceFrame(980000, 1980000, 198),
                    makeSequenceFrame(990000, 1990000, 199)};
  ok &= expect(
      sequenceCache.commitDecodedRun(
          playback_video_frame_step::Direction::Previous,
          secondClipJoin->identity, secondClipJoin, firstClipTail),
      "A previous-frame cache run must join adjacent presentation clips "
      "across a removed source interval");
  playback_video_frame_step_prefetch::FrameWindow sequenceWindow =
      sequenceCache.windowAround(secondClipJoin->identity, 1000000, 0, 20);
  ok &= expect(sequenceWindow.valid() && sequenceWindow.anchorIndex == 2 &&
                   sequenceWindow.frames.front()->ptsUs == 980000 &&
                   sequenceWindow.frames.front()->sourcePtsUs == 1980000,
               "Sequence cache traversal must use presentation adjacency "
               "without inventing continuous source timestamps");

  playback_video_frame_step_prefetch::SourceFrameCache forwardSequenceCache;
  auto firstClipJoin = makeSequenceFrame(990000, 1990000, 199);
  std::vector<
      std::shared_ptr<const playback_video_frame_step_prefetch::SourceFrame>>
      secondClipHead{makeSequenceFrame(1000000, 4000000, 400),
                     makeSequenceFrame(1010000, 4010000, 401)};
  ok &= expect(
      forwardSequenceCache.commitDecodedRun(
          playback_video_frame_step::Direction::Next, firstClipJoin->identity,
          firstClipJoin, secondClipHead),
      "A next-frame cache run must join adjacent presentation clips across a "
      "removed source interval");
  playback_video_frame_step_prefetch::FrameWindow duplicatePtsWindow =
      sourceCache.windowAround(join10->identity, 0, 100000, 20);
  ok &= expect(duplicatePtsWindow.valid() &&
                   duplicatePtsWindow.frames.size() == 4 &&
                   duplicatePtsWindow.frames[1]->info.sourceDtsTicks == 101 &&
                   duplicatePtsWindow.frames[2]->info.sourceDtsTicks == 102,
               "Exact adjacency must retain distinct frames sharing one PTS");

  playback_video_frame_cursor::Controller prefetchedCursor;
  prefetchedCursor.resetForSerial(1);
  QueuedFrame prefetchedCurrent{};
  prefetchedCurrent.ptsUs = 100000;
  prefetchedCurrent.sourcePtsUs = 100000;
  prefetchedCurrent.durationUs = 10000;
  prefetchedCurrent.sourceDurationUs = 10000;
  prefetchedCurrent.serial = 1;
  prefetchedCurrent.displayIndex = 1;
  prefetchedCurrent.info.sourcePtsTicks = 10;
  prefetchedCurrent.info.sourceDtsTicks = 10;
  prefetchedCurrent.info.timestamp100ns = prefetchedCurrent.ptsUs * 10;
  VideoFrame prefetchedCurrentFrame{};
  prefetchedCurrentFrame.timestamp100ns = prefetchedCurrent.ptsUs * 10;
  prefetchedCurrentFrame.duration100ns = prefetchedCurrent.durationUs * 10;
  prefetchedCursor.noteDecoded(prefetchedCurrent);
  prefetchedCursor.appendPresented(prefetchedCurrent, prefetchedCurrentFrame);
  ok &= expect(prefetchedCursor.enterFrameStepMode(),
               "The first frame-step request must activate proactive prefetch");
  std::optional<playback_video_frame_step_prefetch::Request> previousRequest =
      prefetchedCursor.prefetchRequest(
          playback_video_frame_step::Direction::Previous, 5000000);
  ok &= expect(previousRequest && !previousRequest->joinCached &&
                   previousRequest->direction ==
                       playback_video_frame_step::Direction::Previous &&
                   previousRequest->join.ptsUs == 100000 &&
                   previousRequest->rangeStartUs == 0,
               "Initial backward prefetch must decode to the exact current "
               "frame join");
  playback_video_frame_step_prefetch::Result previousResult;
  if (previousRequest) {
    previousResult.request = *previousRequest;
    previousResult.joinFrame = makeTickFrame(10, 10, 42);
  }
  for (int64_t tick = 0; tick < 10; ++tick) {
    previousResult.frames.push_back(makeTickFrame(tick));
  }
  previousResult.reachedMediaBoundary = true;
  ok &= expect(
      prefetchedCursor.adoptPrefetchedResult(std::move(previousResult)) &&
          prefetchedCursor.prefetchWindow().beforeFrameCount == 10 &&
          prefetchedCursor.prefetchWindow().afterFrameCount == 0,
      "Backward prefetch must incrementally publish its exact "
      "decoded prefix");

  std::optional<playback_video_frame_step_prefetch::Request> guardRequest =
      prefetchedCursor.prefetchRequest(
          playback_video_frame_step::Direction::Next, 5000000, 0);
  ok &= expect(guardRequest && guardRequest->joinCached &&
                   guardRequest->join.ptsUs == 100000,
               "After the active prefix, the inverse guard must extend from "
               "the cached current frame");
  playback_video_frame_step_prefetch::Result guardResult;
  if (guardRequest) {
    guardResult.request = *guardRequest;
  }
  guardResult.frames = {makeSourceFrame(110000, 10000, 11, 101),
                        makeSourceFrame(110000, 10000, 11, 102),
                        makeTickFrame(12),
                        makeTickFrame(13),
                        makeTickFrame(14),
                        makeTickFrame(15)};
  ok &= expect(prefetchedCursor.adoptPrefetchedResult(std::move(guardResult)),
               "The inverse guard must append without replacing the prefix");

  playback_video_frame_cursor::PrefetchWindow adoptedPrefetchWindow =
      prefetchedCursor.prefetchWindow();
  ok &= expect(adoptedPrefetchWindow.beforeFrameCount == 10 &&
                   adoptedPrefetchWindow.afterFrameCount == 6,
               "Both decoded sides must remain connected around the current "
               "frame");
  const playback_video_frame_cursor::PresentedFrame* cachedStep =
      prefetchedCursor.step(playback_video_frame_step::Direction::Previous);
  ok &= expect(cachedStep && cachedStep->ptsUs == 90000,
               "Previous-frame must consume the adjacent prefetched frame "
               "without a serial seek");
  cachedStep =
      prefetchedCursor.step(playback_video_frame_step::Direction::Previous);
  ok &= expect(cachedStep && cachedStep->ptsUs == 80000,
               "Repeated previous-frame must continue through the prefetched "
               "window");
  prefetchedCursor.step(playback_video_frame_step::Direction::Next);
  cachedStep =
      prefetchedCursor.step(playback_video_frame_step::Direction::Next);
  ok &= expect(cachedStep && cachedStep->videoFrame().width == 42,
               "The exact join snapshot must detach the current frame from the "
               "primary decoder surface pool");
  const playback_video_frame_cursor::PresentedFrame* firstDuplicate =
      prefetchedCursor.step(playback_video_frame_step::Direction::Next);
  const playback_video_frame_cursor::PresentedFrame* secondDuplicate =
      prefetchedCursor.step(playback_video_frame_step::Direction::Next);
  ok &= expect(firstDuplicate && secondDuplicate &&
                   firstDuplicate->ptsUs == secondDuplicate->ptsUs &&
                   firstDuplicate->info.sourceDtsTicks == 101 &&
                   secondDuplicate->info.sourceDtsTicks == 102,
               "Prefetch identities must preserve distinct decoded frames "
               "that share a presentation timestamp");
  QueuedFrame stalePrimaryNext{};
  stalePrimaryNext.ptsUs = 100000;
  stalePrimaryNext.durationUs = 10000;
  stalePrimaryNext.serial = 1;
  stalePrimaryNext.displayIndex = 1000;
  stalePrimaryNext.info.sourcePtsTicks = 10;
  stalePrimaryNext.info.sourceDtsTicks = 10;
  QueuedFrame freshPrimaryNext = stalePrimaryNext;
  freshPrimaryNext.ptsUs = 120000;
  freshPrimaryNext.info.sourcePtsTicks = 12;
  freshPrimaryNext.info.sourceDtsTicks = 12;
  ok &=
      expect(prefetchedCursor.decodedNextIsStale(stalePrimaryNext) &&
                 !prefetchedCursor.decodedNextIsStale(freshPrimaryNext),
             "Primary-queue fallback must compare source position, not rebased "
             "display indices, after cached stepping");

  playback_video_frame_cursor::Controller failedCursor;
  failedCursor.resetForSerial(3);
  QueuedFrame failedCurrent = prefetchedCurrent;
  failedCurrent.ptsUs = 1000000;
  failedCurrent.serial = 3;
  failedCurrent.info.timestamp100ns = failedCurrent.ptsUs * 10;
  failedCurrent.info.sourcePtsTicks = 100;
  failedCurrent.info.sourceDtsTicks = 100;
  VideoFrame failedCurrentFrame = prefetchedCurrentFrame;
  failedCurrentFrame.timestamp100ns = failedCurrent.ptsUs * 10;
  failedCursor.noteDecoded(failedCurrent);
  failedCursor.appendPresented(failedCurrent, failedCurrentFrame);
  failedCursor.enterFrameStepMode();
  auto failedRequest = failedCursor.prefetchRequest(
      playback_video_frame_step::Direction::Previous, 5000000);
  ok &= expect(failedRequest.has_value(),
               "The cursor must plan an uncached backward run");
  if (failedRequest) {
    failedCursor.notePrefetchFailure(*failedRequest);
  }
  ok &= expect(
      !failedCursor
              .prefetchRequest(playback_video_frame_step::Direction::Previous,
                               5000000)
              .has_value() &&
          failedCursor
              .prefetchRequest(playback_video_frame_step::Direction::Next,
                               5000000)
              .has_value(),
      "A failed exact run must be suppressed without blocking the opposite "
      "direction");

  playback_video_frame_cursor::Controller missingJoinCursor;
  missingJoinCursor.resetForSerial(4);
  QueuedFrame missingJoinCurrent = failedCurrent;
  missingJoinCurrent.serial = 4;
  missingJoinCursor.noteDecoded(missingJoinCurrent);
  missingJoinCursor.appendPresented(missingJoinCurrent, failedCurrentFrame);
  missingJoinCursor.enterFrameStepMode();
  auto missingJoinRequest = missingJoinCursor.prefetchRequest(
      playback_video_frame_step::Direction::Previous, 5000000);
  playback_video_frame_step_prefetch::Result missingJoinResult;
  if (missingJoinRequest) {
    missingJoinResult.request = *missingJoinRequest;
  }
  missingJoinResult.frames.push_back(makeTickFrame(99));
  ok &= expect(
      !missingJoinCursor.adoptPrefetchedResult(std::move(missingJoinResult)) &&
          missingJoinCursor.prefetchWindow().current.ptsUs == 1000000,
      "A worker result without its exact join frame must be rejected "
      "transactionally");

  playback_video_frame_cursor::Controller previousRefillCursor;
  previousRefillCursor.resetForSerial(5);
  QueuedFrame previousRefillCurrent = failedCurrent;
  previousRefillCurrent.serial = 5;
  previousRefillCursor.noteDecoded(previousRefillCurrent);
  previousRefillCursor.appendPresented(previousRefillCurrent,
                                       failedCurrentFrame);
  previousRefillCursor.enterFrameStepMode();
  auto longPreviousRequest = previousRefillCursor.prefetchRequest(
      playback_video_frame_step::Direction::Previous, 5000000);
  playback_video_frame_step_prefetch::Result longPreviousResult;
  if (longPreviousRequest) {
    longPreviousResult.request = *longPreviousRequest;
    longPreviousResult.joinFrame = makeTickFrame(100);
  }
  for (int64_t tick = 0; tick < 100; ++tick) {
    longPreviousResult.frames.push_back(makeTickFrame(tick));
  }
  longPreviousResult.reachedMediaBoundary = true;
  ok &= expect(
      previousRefillCursor.adoptPrefetchedResult(
          std::move(longPreviousResult)) &&
          previousRefillCursor.prefetchWindow().beforeFrameCount == 100,
      "A full backward horizon must be available without a transport seek");
  const auto* previousCachedStep =
      previousRefillCursor.step(playback_video_frame_step::Direction::Previous);
  ok &= expect(previousCachedStep && previousCachedStep->ptsUs == 990000,
               "Backward stepping must consume the immediate cached frame");

  auto forwardRefillRequest = previousRefillCursor.prefetchRequest(
      playback_video_frame_step::Direction::Next, 5000000);
  ok &= expect(forwardRefillRequest && forwardRefillRequest->joinCached &&
                   forwardRefillRequest->join.ptsUs == 1000000,
               "A direction reversal must extend from the existing forward "
               "edge instead of replacing the cache");
  playback_video_frame_step_prefetch::Result forwardRefillResult;
  if (forwardRefillRequest) {
    forwardRefillResult.request = *forwardRefillRequest;
  }
  for (int64_t tick = 101; tick < 200; ++tick) {
    forwardRefillResult.frames.push_back(makeTickFrame(tick));
  }
  ok &= expect(previousRefillCursor.adoptPrefetchedResult(
                   std::move(forwardRefillResult)),
               "A forward refill must append to the exact cached join");
  const auto* reversibleNext =
      previousRefillCursor.step(playback_video_frame_step::Direction::Next);
  const auto* reversiblePrevious =
      previousRefillCursor.step(playback_video_frame_step::Direction::Previous);
  ok &= expect(reversibleNext && reversiblePrevious &&
                   reversibleNext->ptsUs == 1000000 &&
                   reversiblePrevious->ptsUs == 990000,
               "Direction changes must preserve both immediate inverse "
               "neighbors");

  while (previousRefillCursor.prefetchWindow().afterFrameCount >
         playback_video_frame_step_prefetch::kRefillLeadFrameCount) {
    previousRefillCursor.step(playback_video_frame_step::Direction::Next);
  }
  ok &= expect(
      previousRefillCursor
          .prefetchRequest(playback_video_frame_step::Direction::Next, 5000000)
          .has_value(),
      "The cursor must request an incremental refill before the "
      "connected forward edge is exhausted");

  playback_video_frame_step_seek::Controller stepSeekHandoff;
  playback_video_frame_step_seek::Plan handoffSeekPlan;
  handoffSeekPlan.direction = playback_video_frame_step::Direction::Previous;
  handoffSeekPlan.target.ptsUs = 40000;
  handoffSeekPlan.target.durationUs = 10000;
  handoffSeekPlan.target.logicalIndex = 4;
  handoffSeekPlan.anchor = handoffSeekPlan.target;
  stepSeekHandoff.publishForSerial(9, handoffSeekPlan);
  ok &= expect(!stepSeekHandoff.consumeForSerial(8),
               "Frame-step seek handoff must not publish a plan before its "
               "serial is active");
  auto consumedStepSeek = stepSeekHandoff.consumeForSerial(9);
  ok &= expect(consumedStepSeek &&
                   consumedStepSeek->target.logicalIndex == 4,
               "Frame-step seek handoff must publish the exact plan for the "
               "matching serial");
  ok &= expect(!stepSeekHandoff.consumeForSerial(9),
               "Frame-step seek handoff must consume each serial plan once");
  stepSeekHandoff.publishForSerial(10, handoffSeekPlan);
  ok &= expect(!stepSeekHandoff.consumeForSerial(11),
               "Frame-step seek handoff must discard stale plans after serial "
               "advance");
  ok &= expect(!stepSeekHandoff.consumeForSerial(10),
               "Discarded frame-step seek plans must not reappear");

  playback_video_serial_control::Controller serialSeekControl;
  playback_video_serial_control::TransitionPlan providedDemuxSeek =
      serialSeekControl.beginTransition(40000, 12000, true, true);
  ok &= expect(providedDemuxSeek.valid &&
                   providedDemuxSeek.displayTargetUs == 40000 &&
                   providedDemuxSeek.demuxTargetUs == 12000 &&
                   providedDemuxSeek.demuxWindowEndUs == 40000 &&
                   providedDemuxSeek.decoderPrerollTargetUs == 40000 &&
                   providedDemuxSeek.demuxSeekMode ==
                       playback_video_serial_control::DemuxSeekMode::Timeline,
               "Serial seek control must preserve a caller-provided demux "
               "target while normal seek preroll still targets display time");
  playback_video_serial_control::TransitionPlan frameStepPrerollSeek =
      serialSeekControl.beginTransition(
          40000, 12000, 40000, 12000,
          playback_video_serial_control::DemuxSeekMode::VideoAtOrBefore, true,
          true);
  ok &= expect(frameStepPrerollSeek.valid &&
                   frameStepPrerollSeek.displayTargetUs == 40000 &&
                   frameStepPrerollSeek.demuxTargetUs == 12000 &&
                   frameStepPrerollSeek.demuxWindowEndUs == 40000 &&
                   frameStepPrerollSeek.decoderPrerollTargetUs == 12000 &&
                   frameStepPrerollSeek.demuxSeekMode ==
                       playback_video_serial_control::DemuxSeekMode::
                           VideoAtOrBefore,
               "Frame-step redecode must keep decoder preroll at the anchor "
               "and use video-stream seek so cursor-owned frame counting can "
               "see the target sequence");
  playback_video_serial_control::TransitionPlan previousDiscoverySeek =
      serialSeekControl.beginTransition(
          40000, 12000, 12000, 0,
          playback_video_serial_control::DemuxSeekMode::VideoBeforeTarget,
          true, true);
  ok &= expect(previousDiscoverySeek.valid &&
                   previousDiscoverySeek.displayTargetUs == 40000 &&
                   previousDiscoverySeek.demuxTargetUs == 12000 &&
                   previousDiscoverySeek.demuxWindowEndUs == 12000 &&
                   previousDiscoverySeek.decoderPrerollTargetUs == 0 &&
                   previousDiscoverySeek.demuxSeekMode ==
                       playback_video_serial_control::DemuxSeekMode::
                           VideoBeforeTarget,
               "Previous-frame discovery must carry its demux window separately "
               "from the display target and force a strict video-stream seek");
  playback_video_serial_control::PositionSnapshot pendingPosition =
      serialSeekControl.positionSnapshot();
  ok &= expect(pendingPosition.currentSerial == previousDiscoverySeek.serial &&
                   pendingPosition.seekPending &&
                   pendingPosition.pendingSeekSerial ==
                       previousDiscoverySeek.serial &&
                   pendingPosition.seekDisplayUs == 40000,
               "Published playback position must expose one coherent serial, "
               "seek-pending state, and display target");
  playback_video_serial_control::PendingSeek frameStepPendingSeek =
      serialSeekControl.claimPendingSeek();
  ok &= expect(frameStepPendingSeek.valid &&
                   frameStepPendingSeek.displayTargetUs == 40000 &&
                   frameStepPendingSeek.demuxTargetUs == 12000 &&
                   frameStepPendingSeek.demuxWindowEndUs == 12000 &&
                   frameStepPendingSeek.decoderPrerollTargetUs == 0 &&
                   frameStepPendingSeek.demuxSeekMode ==
                       playback_video_serial_control::DemuxSeekMode::
                           VideoBeforeTarget,
               "Claimed previous-frame discovery seeks must carry the demux "
               "window, decoder preroll, and video-stream seek contract across "
               "the demux boundary");
  playback_video_serial_control::PositionSnapshot claimedPosition =
      serialSeekControl.positionSnapshot();
  ok &= expect(!claimedPosition.seekPending &&
                   claimedPosition.pendingSeekSerial ==
                       frameStepPendingSeek.serial &&
                   claimedPosition.seekDisplayUs == 40000,
               "Claiming demux work must retain the published seek position "
               "until a frame acknowledges presentation");
  ok &= expect(!serialSeekControl.clearPendingPresentation(
                   frameStepPendingSeek.serial - 1),
               "A stale frame must not acknowledge seek presentation");
  ok &= expect(serialSeekControl.clearPendingPresentation(
                   frameStepPendingSeek.serial),
               "The current seek frame must acknowledge presentation");
  playback_video_serial_control::PositionSnapshot presentedPosition =
      serialSeekControl.positionSnapshot();
  ok &= expect(!presentedPosition.seekPending &&
                   presentedPosition.pendingSeekSerial == 0 &&
                   presentedPosition.seekDisplayUs == 0,
               "Presentation acknowledgement must atomically retire the seek "
               "display position");
  ok &= expect(!serialSeekControl.clearPendingPresentation(
                   frameStepPendingSeek.serial),
               "Seek presentation acknowledgement must be single-shot");

  playback_video_serial_control::Controller timelineControl;
  timelineControl.startSession(4);
  timelineControl.notePresentedPosition(4, 60000000);
  playback_video_serial_control::PositionSnapshot initialTimeline =
      timelineControl.positionSnapshot();
  ok &= expect(initialTimeline.positionUs == 60000000 &&
                   !initialTimeline.requestPending &&
                   !initialTimeline.transitionPending,
               "The transport timeline must begin at the presented frame");

  const playback_video_serial_control::SeekRequest firstRequest =
      timelineControl.publishSeekRequest(65000000, 120000000);
  playback_video_serial_control::PositionSnapshot firstRequestedTimeline =
      timelineControl.positionSnapshot();
  ok &= expect(firstRequest.generation == 1 &&
                   firstRequest.targetUs == 65000000 &&
                   firstRequestedTimeline.requestPending &&
                   firstRequestedTimeline.positionUs == firstRequest.targetUs,
               "Publishing a seek must synchronously make its target the "
               "authoritative transport position");

  const playback_video_serial_control::SeekRequest secondRequest =
      timelineControl.publishRelativeSeekRequest(5000000, 120000000);
  playback_video_serial_control::PositionSnapshot secondRequestedTimeline =
      timelineControl.positionSnapshot();
  ok &= expect(secondRequest.generation == firstRequest.generation + 1 &&
                   secondRequest.targetUs == 70000000 &&
                   secondRequestedTimeline.positionUs ==
                       secondRequest.targetUs,
               "Consecutive relative seeks must accumulate from the latest "
               "published target, not from a stale presented frame");

  timelineControl.acknowledgeSeekRequest(firstRequest.generation);
  playback_video_serial_control::PositionSnapshot staleAcknowledgement =
      timelineControl.positionSnapshot();
  ok &= expect(staleAcknowledgement.requestPending &&
                   staleAcknowledgement.positionUs == secondRequest.targetUs,
               "Acknowledging an older coalesced seek must not retire a newer "
               "transport request");

  const playback_video_serial_control::TransitionPlan requestedTransition =
      timelineControl.beginTransition(secondRequest.targetUs, true, true);
  timelineControl.acknowledgeSeekRequest(secondRequest.generation);
  playback_video_serial_control::PositionSnapshot transitioningTimeline =
      timelineControl.positionSnapshot();
  ok &= expect(requestedTransition.valid &&
                   requestedTransition.serial == 5 &&
                   !transitioningTimeline.requestPending &&
                   transitioningTimeline.transitionPending &&
                   transitioningTimeline.positionUs == secondRequest.targetUs,
               "The accepted seek target must remain authoritative throughout "
               "its serial transition");

  const playback_video_serial_control::PendingSeek requestedPendingSeek =
      timelineControl.claimPendingSeek();
  ok &= expect(requestedPendingSeek.valid &&
                   requestedPendingSeek.serial == requestedTransition.serial &&
                   timelineControl.applySeekResult(requestedPendingSeek.serial,
                                                   0),
               "The authoritative request must cross the demux handoff on its "
               "assigned serial");
  timelineControl.notePresentedPosition(4, 61000000);
  playback_video_serial_control::PositionSnapshot stalePresentation =
      timelineControl.positionSnapshot();
  ok &= expect(stalePresentation.positionUs == secondRequest.targetUs &&
                   stalePresentation.presentedPositionSerial == 4,
               "A stale serial must not replace the position of an active "
               "transport transition");

  constexpr int64_t kAcceptedFramePtsUs = 70033333;
  timelineControl.notePresentedPosition(requestedTransition.serial,
                                        kAcceptedFramePtsUs);
  ok &= expect(timelineControl.clearPendingPresentation(
                   requestedTransition.serial),
               "The accepted serial must retire its presentation boundary");
  playback_video_serial_control::PositionSnapshot completedTimeline =
      timelineControl.positionSnapshot();
  ok &= expect(!completedTimeline.requestPending &&
                   !completedTimeline.transitionPending &&
                   completedTimeline.positionUs == kAcceptedFramePtsUs &&
                   completedTimeline.presentedPositionSerial ==
                       requestedTransition.serial,
               "After presentation, the decoded frame must become the sole "
               "authoritative transport position");

  const playback_video_serial_control::SeekRequest clampedRequest =
      timelineControl.publishRelativeSeekRequest(
          (std::numeric_limits<int64_t>::min)(), 120000000);
  ok &= expect(clampedRequest.targetUs == 0,
               "Relative transport seeks must clamp safely at timeline zero");

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
  ok &= expect(defaultNonFullscreenPresentation(
                   PlaybackPresentationFamily::Framebuffer) ==
                   PlaybackPresentationMode::PictureInPicture,
               "Framebuffer non-fullscreen default must be PiP");
  ok &= expect(playback_overlay::overlayCellCountForPixels(719, 21) == 35,
               "overlayCellCountForPixels must round rows up");
  ok &= expect(playback_overlay::overlayCellCountForPixels(960, 9) == 107,
               "overlayCellCountForPixels must round columns up");

  ok &= expect(!playback_overlay::isBackMousePressed(
                   makeMouse(RIGHTMOST_BUTTON_PRESSED)),
               "Right mouse button must not act as playback back/exit");
  ok &= expect(playback_overlay::isBackMousePressed(
                   makeMouse(FROM_LEFT_2ND_BUTTON_PRESSED)),
               "Side/back mouse button must still act as playback back/exit");
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

  playback_video_state_machine::PipelineSnapshot pausedSeekBeforeFrame{};
  pausedSeekBeforeFrame.audioPaused = true;
  pausedSeekBeforeFrame.currentSerial = 2;
  pausedSeekBeforeFrame.lastPresentedSerial = 1;
  pausedSeekBeforeFrame.pendingSeekSerial = 2;
  pausedSeekBeforeFrame.seekInFlightSerial = 0;

  playback_video_state_machine::PipelineSnapshot pausedSeekAfterFrame =
      pausedSeekBeforeFrame;
  pausedSeekAfterFrame.lastPresentedSerial = 2;
  pausedSeekAfterFrame.pendingSeekSerial = 0;

  playback_video_state_machine::PipelineSnapshot failedPausedSeek =
      pausedSeekBeforeFrame;
  failedPausedSeek.seekFailed = true;
  failedPausedSeek.pendingSeekSerial = 0;

  playback_video_state_machine::PipelineSnapshot playingSeek = pausedSeekBeforeFrame;
  playingSeek.audioPaused = false;

  playback_video_state_machine::PipelineSnapshot openingReady{};
  openingReady.initDone = true;
  openingReady.initOk = true;
  openingReady.videoQueueDepth = 1;
  openingReady.videoQueueEmpty = false;
  auto putControllerInSeeking = [&](playback_video_state_machine::Controller& c) {
    c.beginOpening();
    c.observe(openingReady, 1);
    c.finishPriming();
    c.beginSeeking();
  };
  auto putControllerInEnded = [&](playback_video_state_machine::Controller& c) {
    c.beginOpening();
    c.observe(openingReady, 1);
    c.finishPriming();
    playback_video_state_machine::PipelineSnapshot endedSnapshot =
        openingReady;
    endedSnapshot.decodeEnded = true;
    endedSnapshot.demuxEnded = true;
    endedSnapshot.videoQueueDepth = 0;
    endedSnapshot.videoQueueEmpty = true;
    endedSnapshot.lastPresentedSerial = endedSnapshot.currentSerial;
    playback_video_state_machine::Evaluation ended =
        c.observe(endedSnapshot, 1);
    ok &= expect(ended.change.changed &&
                     ended.change.current == PlayerState::Ended,
                 "Playback controller test setup must reach Ended");
  };

  playback_video_state_machine::Controller pausedSeekPendingController;
  putControllerInSeeking(pausedSeekPendingController);
  playback_video_state_machine::Evaluation pausedSeekPending =
      pausedSeekPendingController.observe(pausedSeekBeforeFrame, 1);
  ok &= expect(!pausedSeekPending.change.changed &&
                   pausedSeekPending.change.current == PlayerState::Seeking,
               "Paused seek must remain Seeking while a preview frame is "
               "pending");

  playback_video_state_machine::Controller pausedSeekDoneController;
  putControllerInSeeking(pausedSeekDoneController);
  playback_video_state_machine::Evaluation pausedSeekDone =
      pausedSeekDoneController.observe(pausedSeekAfterFrame, 1);
  ok &= expect(pausedSeekDone.change.changed &&
                   pausedSeekDone.change.current == PlayerState::Paused,
               "Paused seek must settle back to Paused after the preview frame "
               "is shown");

  playback_video_state_machine::Controller failedSeekController;
  putControllerInSeeking(failedSeekController);
  playback_video_state_machine::Evaluation failedSeek =
      failedSeekController.observe(failedPausedSeek, 1);
  ok &= expect(failedSeek.change.changed &&
                   failedSeek.change.current == PlayerState::Paused,
               "Failed paused seek must fall back to Paused");

  playback_video_state_machine::Controller playingSeekController;
  putControllerInSeeking(playingSeekController);
  playback_video_state_machine::Evaluation playingSeekDone =
      playingSeekController.observe(playingSeek, 1);
  ok &= expect(playingSeekDone.change.changed &&
                   playingSeekDone.change.current == PlayerState::Prefill,
               "Active seeks must continue into Prefill");

  playback_video_state_machine::Controller endedSeekController;
  putControllerInEnded(endedSeekController);
  playback_video_state_machine::StateChange endedSeek =
      endedSeekController.beginSeeking();
  ok &= expect(endedSeek.changed &&
                   endedSeek.previous == PlayerState::Ended &&
                   endedSeek.current == PlayerState::Seeking,
               "Explicit seeks must leave Ended through Seeking");
  playback_video_state_machine::Evaluation endedSeekDone =
      endedSeekController.observe(playingSeek, 1);
  ok &= expect(endedSeekDone.change.changed &&
                   endedSeekDone.change.current == PlayerState::Prefill,
               "Ended playback restart must continue through Prefill after "
               "the seek is applied");

  ok &= expect(playback_video_state_machine::stateEffects(PlayerState::Seeking)
                   .holdAudioOutput,
               "Seeking must hold audio until clocks are reacquired");
  ok &= expect(!playback_video_state_machine::stateEffects(PlayerState::Playing)
                    .holdAudioOutput,
               "Playing must release audio output hold");
  ok &= expect(playback_video_state_machine::stateEffects(PlayerState::Paused)
                   .pauseMainClock,
               "Paused state must pause the main clock");
  playback_video_state_machine::StateProjection frameStepProjection =
      playback_video_state_machine::project(PlayerState::FrameStep);
  ok &= expect(frameStepProjection.session ==
                       playback_video_state_machine::SessionState::Started &&
                   frameStepProjection.transport ==
                       playback_video_state_machine::TransportState::Paused &&
                   frameStepProjection.buffering ==
                       playback_video_state_machine::BufferingState::Ready &&
                   frameStepProjection.effects.pauseMainClock &&
                   !frameStepProjection.effects.holdAudioOutput,
               "Frame-step must be an explicit paused transport state without "
               "audio-output hold");
  ok &= expect(!playback_video_state_machine::stateEffects(PlayerState::Playing)
                    .pauseMainClock,
               "Playing state must run the main clock");
  playback_video_state_machine::StateProjection playingProjection =
      playback_video_state_machine::project(PlayerState::Playing);
  ok &= expect(
      playingProjection.session ==
              playback_video_state_machine::SessionState::Started &&
          playingProjection.transport ==
              playback_video_state_machine::TransportState::Playing &&
          playingProjection.buffering ==
              playback_video_state_machine::BufferingState::Ready,
      "Playing must project to started/playing/ready");
  ok &= expect(playingProjection.effects.mayPresentVideo,
               "Ready playback must allow video presentation");
  playback_video_state_machine::StateProjection prefillProjection =
      playback_video_state_machine::project(PlayerState::Prefill);
  ok &= expect(prefillProjection.effects.holdAudioOutput &&
                   prefillProjection.effects.pauseMainClock &&
                   !prefillProjection.effects.mayPresentVideo,
               "Prefill must hold outputs until both streams are ready");
  ok &= expect(pausedSeekDone.change.current == PlayerState::Paused &&
                   pausedSeekDone.clearSeekFailure,
               "Leaving Seeking after seek completion must clear seek failure "
               "state centrally");
  playback_video_state_machine::Controller stateController;
  playback_video_state_machine::StateChange openingChange =
      stateController.beginOpening();
  ok &= expect(openingChange.changed &&
                   openingChange.previous == PlayerState::Idle &&
                   openingChange.current == PlayerState::Opening &&
                   openingChange.projection.session ==
                       playback_video_state_machine::SessionState::Opening,
               "State controller must own the Opening transition");
  playback_video_state_machine::StateChange earlySeek =
      stateController.beginSeeking();
  ok &= expect(!earlySeek.changed && earlySeek.current == PlayerState::Opening,
               "Seeking must not start before the session is started");
  playback_video_state_machine::Evaluation observedOpening =
      stateController.observe(openingReady, 1);
  ok &= expect(observedOpening.change.changed &&
                   observedOpening.change.current == PlayerState::Priming,
               "Observed pipeline readiness must update the controller");
  playback_video_state_machine::StateChange seekChange =
      stateController.beginSeeking();
  ok &= expect(seekChange.changed &&
                   seekChange.current == PlayerState::Seeking &&
                   seekChange.projection.transport ==
                       playback_video_state_machine::TransportState::Seeking,
               "State controller must own the Seeking transition");
  playback_video_state_machine::StateChange invalidPrimingFinish =
      stateController.finishPriming();
  ok &= expect(!invalidPrimingFinish.changed &&
                   invalidPrimingFinish.current == PlayerState::Seeking,
               "Priming completion must be ignored outside Priming");
  playback_video_state_machine::Evaluation observedSeek =
      stateController.observe(playingSeek, 1);
  ok &= expect(observedSeek.change.changed &&
                   observedSeek.change.current == PlayerState::Prefill,
               "Observed active seek completion must enter Prefill");
  playback_video_state_machine::Evaluation observedPrefill =
      stateController.observe(openingReady, 1);
  ok &= expect(observedPrefill.change.changed &&
                   observedPrefill.change.current == PlayerState::Priming,
               "Observed ready prefill must enter Priming");
  playback_video_state_machine::StateChange primingFinish =
      stateController.finishPriming();
  ok &= expect(primingFinish.changed &&
                   primingFinish.current == PlayerState::Playing &&
                   primingFinish.projection.buffering ==
                       playback_video_state_machine::BufferingState::Ready,
               "State controller must own the priming-to-playing transition");

  playback_video_state_machine::Controller shortSequenceController;
  playback_video_state_machine::PipelineSnapshot shortSequenceReady =
      openingReady;
  shortSequenceReady.audioStartedOk = true;
  shortSequenceReady.audioBufferedFrames =
      playback_video_state_machine::requiredAudioPrefillFrames(48000);
  shortSequenceReady.audioSampleRate = 48000;
  shortSequenceReady.audioSyncPointReady = true;
  shortSequenceController.beginOpening();
  playback_video_state_machine::Evaluation shortSequencePriming =
      shortSequenceController.observe(shortSequenceReady, 1);
  ok &= expect(shortSequencePriming.change.changed &&
                   shortSequencePriming.change.current ==
                       PlayerState::Priming,
               "A short sequence with full prefill must enter Priming");
  playback_video_state_machine::PipelineSnapshot shortSequenceDrained =
      shortSequenceReady;
  shortSequenceDrained.demuxEnded = true;
  shortSequenceDrained.decodeEnded = true;
  shortSequenceDrained.audioDecodeEnded = true;
  shortSequenceDrained.videoQueueDepth = 0;
  shortSequenceDrained.videoQueueEmpty = true;
  playback_video_state_machine::Evaluation shortSequenceDrain =
      shortSequenceController.observe(shortSequenceDrained, 1);
  ok &= expect(shortSequenceDrain.change.changed &&
                   shortSequenceDrain.change.current == PlayerState::Draining,
               "A fully decoded short sequence must release held audio and "
               "enter Draining from Priming");
  shortSequenceDrained.audioFinished = true;
  playback_video_state_machine::Evaluation shortSequenceEnded =
      shortSequenceController.observe(shortSequenceDrained, 1);
  ok &= expect(shortSequenceEnded.change.changed &&
                   shortSequenceEnded.change.current == PlayerState::Ended,
               "A drained short sequence must reach Ended");

  playback_video_sync::LoopState loopState{};
  loopState.frameTimerUs = 1000;
  playback_video_sync::LoopState cadenceLoop{};
  cadenceLoop.lastFrameDurationUs = 33333;
  cadenceLoop.recentDurations = {40000, 40000, 40000};
  QueuedFrame cadenceFrame{};
  cadenceFrame.serial = 1;
  cadenceFrame.ptsUs = 100000;
  cadenceFrame.durationUs = 10000;
  cadenceFrame.displayIndex = 1;
  playback_video_sync::PreparedFrame cadencePrepared =
      playback_video_sync::prepareFrame(cadenceLoop, cadenceFrame, nullptr);
  ok &= expect(cadencePrepared.frameDurationUs == 10000 &&
                   cadencePrepared.delayUs == 28000,
               "Prepared frames must keep media cadence separate from "
               "smoothed presentation delay");
  playback_video_sync::LoopState immediatePresentLoop{};
  immediatePresentLoop.lastFrameDurationUs = 33333;
  playback_video_sync::notePresentedFrame(immediatePresentLoop,
                                          cadencePrepared, 8000, 8000);
  ok &= expect(immediatePresentLoop.lastFrameDurationUs == 10000,
               "Immediate seek presentation must not store scheduler delay "
               "as frame cadence");

  playback_video_sync::PreparedFrame prepared{};
  prepared.frame.ptsUs = 5000;
  prepared.frame.durationUs = 33333;
  playback_video_main_clock::Snapshot master{};
  playback_video_sync::FramePlan seekPlan = playback_video_sync::planFrame(
      loopState, PlayerState::Seeking, prepared, master, 8000);
  ok &= expect(seekPlan.delayUs == 0 && seekPlan.targetUs == 8000,
               "Seeking must present the next frame immediately");
  playback_video_sync::PreparedFrame displayedFrame{};
  displayedFrame.frame.ptsUs = 30000;
  displayedFrame.frame.durationUs = 33333;
  displayedFrame.frame.displayIndex = 3;
  displayedFrame.frameDurationUs = 33333;
  playback_video_sync::notePresentedFrame(loopState, displayedFrame, 8000,
                                          8000);
  QueuedFrame duplicatePtsNext{};
  duplicatePtsNext.ptsUs = 30000;
  duplicatePtsNext.displayIndex = 4;
  ok &= expect(!playback_video_sync::isFrameBackwards(loopState,
                                                      duplicatePtsNext),
               "Frame ordering must use display index instead of duplicate PTS");
  QueuedFrame staleFrame{};
  staleFrame.ptsUs = 40000;
  staleFrame.displayIndex = 3;
  ok &= expect(playback_video_sync::isFrameBackwards(loopState, staleFrame),
               "Frame ordering must reject already-presented display indices");

  playback_video_main_clock::Controller mainClock;
  playback_video_main_clock::SampleRequest clockRequest{};
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
  playback_video_main_clock::Snapshot audioMaster = mainClock.sample(clockRequest);
  ok &= expect(audioMaster.source == PlayerClockSource::Audio,
               "Eligible audio must be selected as the main playback clock");
  ok &= expect(playback_video_main_clock::convertToSystemUs(audioMaster, 20000, 0) ==
                   1010000,
               "Main clock must convert stream timestamps to system time");

  clockRequest.audio.mayDriveMaster = false;
  playback_video_main_clock::Snapshot videoFallback = mainClock.sample(clockRequest);
  ok &= expect(videoFallback.source == PlayerClockSource::Video,
               "Blocked audio master must fall back to the video clock");
  clockRequest.audio.starved = true;
  playback_video_main_clock::Snapshot reacquiringAudio =
      mainClock.sample(clockRequest);
  playback_video_sync::LoopState reacquireLoop{};
  reacquireLoop.frameTimerUs = 1000000;
  playback_video_sync::PreparedFrame reacquireFrame{};
  reacquireFrame.frame.ptsUs = 42333;
  reacquireFrame.frame.durationUs = 33333;
  reacquireFrame.delayUs = 33333;
  reacquireFrame.frameDurationUs = 33333;
  playback_video_sync::FramePlan reacquirePlan = playback_video_sync::planFrame(
      reacquireLoop, PlayerState::Playing, reacquireFrame, reacquiringAudio,
      1000000);
  ok &= expect(!reacquirePlan.waitForAudioRecovery &&
                   reacquirePlan.targetUs > 0,
               "Reacquiring audio must not block video-clock playback");
  clockRequest.audio.starved = false;

  playback_video_sync::LoopState convertedLoop{};
  convertedLoop.frameTimerUs = 500000;
  playback_video_sync::PreparedFrame convertedFrame{};
  convertedFrame.frame.ptsUs = 20000;
  convertedFrame.frame.durationUs = 33333;
  convertedFrame.delayUs = 33333;
  convertedFrame.frameDurationUs = 33333;
  playback_video_sync::FramePlan convertedPlan = playback_video_sync::planFrame(
      convertedLoop, PlayerState::Playing, convertedFrame, audioMaster, 1000000);
  ok &= expect(convertedPlan.targetUs == 1010000,
               "Video scheduling must use the main clock's stream-to-system "
               "conversion");

  playback_video_sync::LoopState driftLoop{};
  driftLoop.frameTimerUs = 700000;
  driftLoop.lastFrameDurationUs = 33333;
  for (int i = 1; i <= 5; ++i) {
    playback_video_main_clock::Snapshot movingAudioMaster = audioMaster;
    movingAudioMaster.us = 10000 + (i - 1) * 33333;
    movingAudioMaster.systemUs = 1000000 + (i - 1) * 33333;

    playback_video_sync::PreparedFrame driftFrame{};
    driftFrame.frame.ptsUs = 10000 + i * 33333;
    driftFrame.frame.durationUs = 33333;
    driftFrame.frame.displayIndex = static_cast<uint64_t>(i);
    driftFrame.delayUs = 33333;
    driftFrame.frameDurationUs = 33333;
    playback_video_sync::FramePlan driftPlan = playback_video_sync::planFrame(
        driftLoop, PlayerState::Playing, driftFrame, movingAudioMaster,
        movingAudioMaster.systemUs);
    int64_t expectedTargetUs = 1000000 + i * 33333;
    ok &= expect(driftPlan.targetUs == expectedTargetUs,
                 "Audio-master scheduling must not accumulate local timer drift");
    playback_video_sync::notePresentedFrame(
        driftLoop, driftFrame, driftPlan.targetUs,
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
