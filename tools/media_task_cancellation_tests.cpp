#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "playback/session/media_task_cancellation.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "media_task_cancellation_tests: " << message << '\n';
  return false;
}

}  // namespace

int main() {
  using Action = playback_media_actions::Action;
  using Choice = playback_session::MediaTaskCancellationChoice;
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

  playback_session::MediaTaskCancellationPromptState prompt;
  ok &= expect(separation && prompt.open(*separation) && prompt.snapshot() &&
                   prompt.snapshot()->selected == Choice::KeepRunning,
               "a playback confirmation must open with the safe choice "
               "selected");
  ok &= expect(!prompt.open(*separation),
               "a modal decision must not be replaced while active");
  ok &= expect(prompt.moveSelection(-1) &&
                   prompt.snapshot()->selected == Choice::CancelTask,
               "keyboard navigation must reach the destructive choice");
  const auto activation = prompt.activate();
  ok &=
      expect(activation && activation->cancelTask &&
                 activation->request.operation == Operation::AudioSeparation &&
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

  playback_media_processing::CancellationRequest invalid{
      playback_media_processing::TaskId{45}, Action::CancelAudioSeparation,
      Operation::AudioExport, source};
  ok &= expect(!prompt.open(std::move(invalid)),
               "an action and operation mismatch must never become a visible "
               "confirmation");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
