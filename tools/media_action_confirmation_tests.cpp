#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

#include "playback/session/media_action_confirmation.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "media_action_confirmation_tests: " << message << '\n';
  return false;
}

class WorkflowService final : public playback_media_processing::Service {
 public:
  playback_media_processing::SourceState sourceStateFor(
      const std::filesystem::path& sourceFile) const override {
    sourceStateQueries.push_back(sourceFile);
    return stateSource.empty() || sourceFile == stateSource
               ? state
               : playback_media_processing::SourceState{};
  }

  playback_media_processing::RequestResult requestSubtitles(
      const std::filesystem::path&) override {
    return rejected();
  }
  playback_media_processing::RequestResult requestSubtitleCancellation()
      override {
    return rejected();
  }
  playback_media_processing::RequestResult requestAudioExport(
      const std::filesystem::path&) override {
    return rejected();
  }
  playback_media_processing::RequestResult requestTranscriptTextExport(
      const std::filesystem::path&) override {
    return rejected();
  }
  playback_media_processing::RequestResult requestMediaExportCancellation()
      override {
    return rejected();
  }
  playback_media_processing::RequestResult requestAudioSeparationSetup(
      const std::filesystem::path&) override {
    ++setupRequests;
    return setupAccepted
               ? playback_media_processing::RequestResult::accepted()
               : playback_media_processing::RequestResult::rejected(
                     playback_media_processing::RequestFailure::
                         BackendUnavailable);
  }
  playback_media_processing::RequestResult
  requestAudioSeparationSetupCancellation() override {
    return rejected();
  }
  playback_media_processing::RequestResult requestAudioSeparation(
      const std::filesystem::path&) override {
    return rejected();
  }
  playback_media_processing::RequestResult
  requestAudioSeparationCancellation() override {
    return rejected();
  }
  playback_media_processing::RequestResult requestTaskCancellation(
      playback_media_processing::TaskId expectedTask) override {
    cancellationAttempts.push_back(expectedTask);
    return state.activeTaskId && *state.activeTaskId == expectedTask
               ? playback_media_processing::RequestResult::accepted()
               : rejected();
  }

  static playback_media_processing::RequestResult rejected() {
    return playback_media_processing::RequestResult::rejected(
        playback_media_processing::RequestFailure::NotRunning);
  }

  playback_media_processing::SourceState state;
  std::filesystem::path stateSource;
  mutable std::vector<std::filesystem::path> sourceStateQueries;
  int setupRequests = 0;
  bool setupAccepted = true;
  std::vector<playback_media_processing::TaskId> cancellationAttempts;
};

}  // namespace

int main() {
  using Action = playback_media_actions::Action;
  using Choice = playback_session::MediaActionConfirmationChoice;
  using Operation = playback_media_processing::Operation;

  bool ok = true;
  const std::filesystem::path source = LR"(C:\Media\NTE.mp4)";

  playback_media_processing::SourceState separationState;
  separationState.activeTaskId = playback_media_processing::TaskId{42};
  separationState.activeTaskCancellable = true;
  separationState.audioSeparationRunning = true;
  const auto separation = playback_media_processing::prepareCancellation(
      Action::CancelAudioSeparation, source, separationState);
  ok &=
      expect(separation &&
                 separation->taskId == playback_media_processing::TaskId{42} &&
                 separation->operation == Operation::AudioSeparation &&
                 separation->sourceFile == source,
             "a visible cancellation action must freeze its exact task, "
             "operation and source");

  playback_media_processing::SourceState exportState;
  exportState.activeTaskId = playback_media_processing::TaskId{43};
  exportState.activeTaskCancellable = true;
  exportState.transcriptTextExportRunning = true;
  const auto transcriptExport = playback_media_processing::prepareCancellation(
      Action::CancelMediaExport, source, exportState);
  exportState.audioExportRunning = true;
  const auto ambiguousExport = playback_media_processing::prepareCancellation(
      Action::CancelMediaExport, source, exportState);
  ok &= expect(
      transcriptExport &&
          transcriptExport->operation == Operation::TranscriptTextExport &&
          !ambiguousExport,
      "generic export cancellation must resolve to one exact active "
      "operation before confirmation");

  exportState.activeTaskCancellable = false;
  ok &= expect(!playback_media_processing::prepareCancellation(
                   Action::CancelMediaExport, source, exportState),
               "a task past its publication barrier must not expose a stale "
               "cancellation intent");

  playback_session::MediaActionConfirmationState prompt;
  ok &= expect(separation && prompt.open(*separation) && prompt.snapshot() &&
                   prompt.snapshot()->selected == Choice::Secondary,
               "a playback confirmation must open with the safe choice "
               "selected");
  ok &= expect(!prompt.open(*separation),
               "a modal decision must not be replaced while active");
  ok &= expect(prompt.moveSelection(-1) &&
                   prompt.snapshot()->selected == Choice::Primary,
               "keyboard navigation must reach the destructive choice");
  const auto activation = prompt.activate();
  const auto* cancellation =
      activation
          ? std::get_if<playback_media_processing::CancellationRequest>(
                &activation->intent)
          : nullptr;
  ok &=
      expect(activation && activation->confirmed && cancellation &&
                 cancellation->operation == Operation::AudioSeparation &&
                 !prompt.snapshot(),
             "only activation of the selected destructive choice may "
             "publish cancellation intent");

  ok &= expect(separation && prompt.open(*separation),
               "the prompt must be reusable for a later decision");
  playback_media_processing::Completion unrelated;
  unrelated.taskId = separation->taskId;
  unrelated.operation = Operation::AudioSeparation;
  unrelated.sourceFile = LR"(C:\Media\Other.mp4)";
  ok &= expect(!prompt.synchronize(unrelated) && prompt.snapshot(),
               "an unrelated task completion must not dismiss the prompt");
  playback_media_processing::Completion completed = unrelated;
  completed.sourceFile = source;
  playback_media_processing::Completion replacement = completed;
  replacement.taskId = playback_media_processing::TaskId{44};
  ok &= expect(!prompt.synchronize(replacement) && prompt.snapshot(),
               "a replacement task with the same operation and source must "
               "not retire the original confirmation");
  ok &= expect(prompt.synchronize(completed) && !prompt.snapshot(),
               "completion of the represented task must retire a stale "
               "confirmation");

  ok &= expect(separation && prompt.open(*separation),
               "the cancellation workflow must permit a fresh decision");
  playback_media_processing::Activity liveActivity;
  liveActivity.taskId = separation->taskId;
  liveActivity.operation = separation->operation;
  liveActivity.sourceFile = separation->sourceFile;
  liveActivity.cancellable = true;
  ok &= expect(!prompt.synchronize(liveActivity) && prompt.snapshot(),
               "an unchanged cancellable task must keep its pending user "
               "decision alive");
  liveActivity.cancellable = false;
  ok &= expect(prompt.synchronize(liveActivity) && !prompt.snapshot(),
               "crossing the task commit barrier must retire its pending "
               "cancellation decision immediately");

  ok &= expect(separation && prompt.open(*separation),
               "a replacement lifecycle check must start from a live task");
  liveActivity.cancellable = true;
  liveActivity.taskId = playback_media_processing::TaskId{46};
  ok &= expect(prompt.synchronize(liveActivity) && !prompt.snapshot(),
               "task replacement must retire a decision for the old task "
               "without waiting for its completion notification");

  playback_media_processing::CancellationRequest invalid{
      {}, Operation::AudioExport, source};
  ok &= expect(!prompt.open(std::move(invalid)),
               "a cancellation without task identity must never become a "
               "visible confirmation");

  playback_media_processing::SourceState setupState;
  setupState.audioSeparationAvailability =
      playback_media_processing::AudioSeparationAvailability::SetupRequired;
  const auto setup = playback_media_processing::prepareAudioSeparationSetup(
      Action::SetUpAudioSeparation, source, setupState);
  ok &= expect(setup && prompt.open(*setup) && prompt.snapshot() &&
                   prompt.snapshot()->selected == Choice::Secondary,
               "provider setup must reuse the safe-default confirmation "
               "workflow");
  const auto declinedSetup = prompt.resolve(Choice::Secondary);
  ok &= expect(declinedSetup && !declinedSetup->confirmed &&
                   !prompt.snapshot() && setup && prompt.open(*setup),
               "declining setup must close the decision without confirming "
               "the operation and permit a later request");
  const auto setupActivation = prompt.resolve(Choice::Primary);
  const auto* confirmedSetup =
      setupActivation
          ? std::get_if<
                playback_media_processing::AudioSeparationSetupRequest>(
                &setupActivation->intent)
          : nullptr;
  ok &= expect(setupActivation && setupActivation->confirmed &&
                   confirmedSetup && confirmedSetup->sourceFile == source,
               "setup confirmation must preserve its typed source intent");

  WorkflowService workflowService;
  workflowService.stateSource = source;
  workflowService.state.audioSeparationAvailability =
      playback_media_processing::AudioSeparationAvailability::SetupRequired;
  playback_media_processing::Actions workflowActions(workflowService);
  const auto workflowSetup = workflowActions.prepareAudioSeparationSetup(
      Action::SetUpAudioSeparation, source);
  playback_session::MediaActionConfirmationState workflowPrompt;
  ok &= expect(workflowSetup && workflowPrompt.open(*workflowSetup),
               "the end-to-end setup workflow must prepare a decision");
  const auto workflowDeclined = workflowPrompt.resolve(Choice::Secondary);
  const auto declinedResult =
      workflowDeclined
          ? playback_session::executeConfirmedMediaAction(
                workflowActions, *workflowDeclined)
          : std::nullopt;
  ok &= expect(workflowDeclined && !declinedResult &&
                   workflowService.setupRequests == 0,
               "declining setup must not invoke the application service");
  ok &= expect(workflowSetup && workflowPrompt.open(*workflowSetup),
               "the setup workflow must be retryable after declining");
  const auto workflowConfirmed = workflowPrompt.resolve(Choice::Primary);
  const auto confirmedResult =
      workflowConfirmed
          ? playback_session::executeConfirmedMediaAction(
                workflowActions, *workflowConfirmed)
          : std::nullopt;
  ok &= expect(confirmedResult && confirmedResult->accepted &&
                   workflowService.setupRequests == 1,
               "confirmation must invoke the shared application action on "
               "the originating surface");

  workflowService.setupAccepted = false;
  ok &= expect(workflowSetup && workflowPrompt.open(*workflowSetup),
               "a failed setup attempt must still begin with confirmation");
  const auto rejectedActivation = workflowPrompt.resolve(Choice::Primary);
  const auto rejectedResult =
      rejectedActivation
          ? playback_session::executeConfirmedMediaAction(
                workflowActions, *rejectedActivation)
          : std::nullopt;
  ok &= expect(rejectedResult && !rejectedResult->accepted &&
                   rejectedResult->error &&
                   rejectedResult->error->failure ==
                       playback_media_processing::RequestFailure::
                           BackendUnavailable &&
                   workflowService.setupRequests == 2,
               "the originating playback workflow must receive a structured "
               "setup-start rejection instead of losing it in browser state");

  workflowService.state = {};
  workflowService.state.activeTaskId =
      playback_media_processing::TaskId{71};
  workflowService.state.activeTaskCancellable = true;
  workflowService.state.audioSeparationSetupRunning = true;
  const auto frozenCancellation = workflowActions.prepareCancellation(
      Action::CancelAudioSeparationSetup, source);
  workflowService.state.activeTaskId =
      playback_media_processing::TaskId{72};
  ok &= expect(frozenCancellation && workflowPrompt.open(*frozenCancellation),
               "cancellation must freeze the task identity before asking");
  const auto staleConfirmation = workflowPrompt.resolve(Choice::Primary);
  const auto staleResult =
      staleConfirmation
          ? playback_session::executeConfirmedMediaAction(
                workflowActions, *staleConfirmation)
          : std::nullopt;
  ok &= expect(staleResult && !staleResult->accepted &&
                   workflowService.cancellationAttempts.size() == 1 &&
                   workflowService.cancellationAttempts.front() ==
                       playback_media_processing::TaskId{71} &&
                   workflowService.state.activeTaskId ==
                       playback_media_processing::TaskId{72},
               "a stale confirmation must target its frozen identity and "
               "leave a replacement task untouched");

  const std::filesystem::path taskSource = LR"(C:\Media\Background.mp4)";
  const std::filesystem::path viewedSource = LR"(C:\Media\NowPlaying.mp4)";
  workflowService.state = {};
  workflowService.stateSource = taskSource;
  workflowService.state.activeTaskId =
      playback_media_processing::TaskId{81};
  workflowService.state.activeTaskCancellable = true;
  workflowService.state.audioSeparationRunning = true;
  playback_media_processing::Activity globalActivity;
  globalActivity.taskId = *workflowService.state.activeTaskId;
  globalActivity.operation = Operation::AudioSeparation;
  globalActivity.sourceFile = taskSource;
  globalActivity.cancellable = true;
  workflowService.sourceStateQueries.clear();
  const auto crossSurfaceCancellation =
      workflowActions.prepareCancellation(globalActivity);
  const bool crossSurfacePromptOpened =
      crossSurfaceCancellation &&
      workflowPrompt.open(*crossSurfaceCancellation);
  const auto crossSurfaceActivation =
      crossSurfacePromptOpened
          ? workflowPrompt.resolve(Choice::Primary)
          : std::nullopt;
  const auto crossSurfaceResult =
      crossSurfaceActivation
          ? playback_session::executeConfirmedMediaAction(
                workflowActions, *crossSurfaceActivation)
          : std::nullopt;
  ok &= expect(
      crossSurfaceCancellation &&
          crossSurfaceCancellation->sourceFile == taskSource &&
          crossSurfaceCancellation->taskId == globalActivity.taskId &&
          taskSource != viewedSource && crossSurfacePromptOpened &&
          crossSurfaceResult && crossSurfaceResult->accepted &&
          workflowService.sourceStateQueries.empty() &&
          workflowService.cancellationAttempts.back() ==
              globalActivity.taskId,
      "a task carried onto another playback surface must resolve direct "
      "cancellation against the task source rather than the viewed media");

  globalActivity.taskId = playback_media_processing::TaskId{82};
  globalActivity.operation = Operation::MelodyAnalysis;
  globalActivity.sourceFile = LR"(C:\Media\Song.flac)";
  workflowService.state.activeTaskId = globalActivity.taskId;
  const auto analysisCancellation =
      workflowActions.prepareCancellation(globalActivity);
  const bool analysisPromptOpened =
      analysisCancellation && workflowPrompt.open(*analysisCancellation);
  const auto analysisActivation =
      analysisPromptOpened ? workflowPrompt.resolve(Choice::Primary)
                           : std::nullopt;
  const auto analysisResult =
      analysisActivation
          ? playback_session::executeConfirmedMediaAction(
                workflowActions, *analysisActivation)
          : std::nullopt;
  ok &= expect(analysisCancellation && analysisPromptOpened &&
                   analysisResult && analysisResult->accepted &&
                   workflowService.cancellationAttempts.back() ==
                       globalActivity.taskId,
               "direct task cancellation must represent generic analysis "
               "work and reach the identity-bound application owner");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
