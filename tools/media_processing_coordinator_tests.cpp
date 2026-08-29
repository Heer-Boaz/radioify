#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/media_processing_actions.h"
#include "app/media_processing_coordinator.h"
#include "playback/media_processing_actions.h"
#include "playback/session/media_task_feedback.h"
#include "tui/ui/media_task_controller.h"
#include "tui/ui/media_task_presentation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "media_processing_coordinator_tests: " << message << '\n';
  return false;
}

template <typename Predicate>
bool waitUntil(Predicate predicate) {
  for (int attempt = 0; attempt < 400; ++attempt) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

std::optional<media_processing::TaskCompletion> waitForCompletion(
    media_processing::Coordinator& coordinator) {
  std::optional<media_processing::TaskCompletion> completion;
  waitUntil([&]() {
    media_processing::PollResult update = coordinator.poll();
    if (!update.completions.empty()) {
      completion = std::move(update.completions.back());
      return true;
    }
    return false;
  });
  return completion;
}

std::optional<media_processing::TaskCompletion> waitForCompletion(
    tui_media_tasks::Controller& controller) {
  std::optional<media_processing::TaskCompletion> completion;
  waitUntil([&]() {
    tui_media_tasks::Update update = controller.poll();
    if (!update.completions.empty()) {
      completion = std::move(update.completions.back());
      return true;
    }
    return false;
  });
  return completion;
}

bool wakeIsSignaled(const media_processing::Coordinator& coordinator) {
  const NativeWaitHandle handle = coordinator.waitHandle();
  return handle &&
         WaitForSingleObject(static_cast<HANDLE>(handle.get()), 0) ==
             WAIT_OBJECT_0;
}

}  // namespace

int main() {
  namespace processing = media_processing;
  bool ok = true;

  std::atomic<bool> melodyStarted{false};
  std::atomic<bool> releaseMelody{false};
  std::atomic<bool> loopStarted{false};
  std::atomic<bool> releaseLoop{false};
  std::atomic<bool> subtitlesStarted{false};
  std::atomic<bool> releaseSubtitles{false};
  std::atomic<bool> separationStarted{false};
  std::atomic<int> separationCheckpoints{0};
  std::atomic<bool> separationCancellationObserved{false};
  std::atomic<bool> releaseSeparation{false};
  std::atomic<bool> audioExportStarted{false};
  std::atomic<bool> releaseAudioExport{false};
  std::atomic<bool> transcriptExportStarted{false};
  std::atomic<bool> transcriptExportCancellationObserved{false};
  std::filesystem::path observedAudioExportOutput;
  std::filesystem::path observedTranscriptExportOutput;
  int observedMelodyTrackIndex = -1;
  std::filesystem::path observedMelodyOutput;
  int observedLoopTrackIndex = -1;
  std::filesystem::path observedStingerOutput;
  std::filesystem::path observedLoopOutput;
  float observedLoopConfidence = 0.0f;

  processing::Coordinator::Operations operations;
  operations.analyzeMelody =
      [&](const std::filesystem::path&, int trackIndex,
          const std::filesystem::path& outputFile,
          const processing::Coordinator::ProgressReporter& progress,
          const processing::Coordinator::CancellationRequested& cancellation,
          const processing::Coordinator::CommitStarted& beginCommit,
          std::string*) {
        observedMelodyTrackIndex = trackIndex;
        observedMelodyOutput = outputFile;
        progress(0.4f, "Analyzing melody");
        melodyStarted.store(true, std::memory_order_release);
        while (!releaseMelody.load(std::memory_order_acquire) &&
               !cancellation()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return !cancellation() && beginCommit();
      };
  operations.splitLoop =
      [&](const std::filesystem::path&,
          const std::filesystem::path& stingerOutput,
          const std::filesystem::path& loopOutput,
          const LoopSplitConfig& config,
          LoopSplitResult* result,
          const processing::Coordinator::ProgressReporter&,
          const processing::Coordinator::CancellationRequested& cancellation,
          const processing::Coordinator::CommitStarted& beginCommit,
          std::string*) {
        observedLoopTrackIndex = config.trackIndex;
        observedStingerOutput = stingerOutput;
        observedLoopOutput = loopOutput;
        observedLoopConfidence = config.minConfidence;
        loopStarted.store(true, std::memory_order_release);
        while (!releaseLoop.load(std::memory_order_acquire) &&
               !cancellation()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (cancellation()) return false;
        if (result) result->hasStinger = true;
        return beginCommit();
      };
  operations.generateSubtitles =
      [&](const std::filesystem::path&, const std::filesystem::path&,
          const playback_video_transcript::GenerationJob::ProgressReporter&
              progress,
          const std::atomic<bool>*,
          const playback_video_transcript::GenerationJob::CommitStarted&
              beginCommit,
          std::string*) {
        progress(0.3f, "Transcribing audio");
        subtitlesStarted.store(true, std::memory_order_release);
        while (!releaseSubtitles.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return beginCommit();
      };
  operations.separateAudio = audio_separation::OperationBinding::ready(
      [&](const std::filesystem::path&,
          const audio_separation::ArtifactPaths&,
          const audio_separation::Job::ProgressReporter& progress,
          const audio_separation::Job::DiagnosticReporter&,
          const audio_separation::ExecutionControl& control,
          const audio_separation::Job::CommitStarted&,
          std::string* error) {
        progress(0.2f, "Separating dialogue, music and effects on GPU");
        separationStarted.store(true, std::memory_order_release);
        while (control.checkpoint()) {
          separationCheckpoints.fetch_add(1, std::memory_order_release);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        separationCancellationObserved.store(true, std::memory_order_release);
        while (!releaseSeparation.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (error) *error = "Controlled cancellation.";
        return false;
      },
      "Test GPU backend");
  operations.exportAudio =
      [&](const std::filesystem::path&,
          const std::filesystem::path& outputFile,
          const processing::Coordinator::ProgressReporter& progress,
          const processing::Coordinator::CancellationRequested& cancellation,
          const processing::Coordinator::CommitStarted& beginCommit,
          std::string*) {
        observedAudioExportOutput = outputFile;
        progress(0.5f, "Extracting lossless audio");
        audioExportStarted.store(true, std::memory_order_release);
        while (!releaseAudioExport.load(std::memory_order_acquire) &&
               !cancellation()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return !cancellation() && beginCommit();
      };
  operations.exportTranscriptText =
      [&](const std::filesystem::path&,
          const std::filesystem::path& outputFile,
          const processing::Coordinator::ProgressReporter& progress,
          const processing::Coordinator::CancellationRequested& cancellation,
          const processing::Coordinator::CommitStarted&,
          std::string*) {
        observedTranscriptExportOutput = outputFile;
        progress(0.6f, "Writing plain-text transcript");
        transcriptExportStarted.store(true, std::memory_order_release);
        while (!cancellation()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        transcriptExportCancellationObserved.store(
            true, std::memory_order_release);
        return false;
      };
  processing::Coordinator coordinator(std::move(operations));

  processing::Actions applicationActions(coordinator);
  playback_media_processing::Actions playbackActions =
      applicationActions.playbackActions();
  MediaTaskPresenter presenter(coordinator);
  const std::optional<playback_media_processing::ActionResult>
      unsupportedAction =
      playbackActions.execute(playback_media_actions::Action::EditVideo,
                              "movie.mp4");

  ok &= expect(static_cast<bool>(coordinator.waitHandle()),
               "all worker families must fan in through one owner wake event");
  ok &= expect(!presenter.activeCard() && !presenter.latestStatus() &&
                   !presenter.latestFailure(),
               "the presenter must not invent inactive task state");
  processing::TaskCompletion failedSeparation;
  failedSeparation.kind = processing::TaskKind::AudioSeparation;
  failedSeparation.outcome = processing::TaskOutcome::Failed;
  failedSeparation.sourceFile = "C:/Media/movie.mp4";
  failedSeparation.diagnosticLog = "C:/Logs/separation.log";
  failedSeparation.detail = "DirectML device was removed";
  const auto failureDialog =
      mediaTaskFailureDialogModel(failedSeparation);
  ok &= expect(
      failureDialog &&
          failureDialog->content.title == "Audio separation failed" &&
          failureDialog->content.text.size() == 4 &&
          failureDialog->content.text[1].text ==
              "Reason: DirectML device was removed" &&
          failureDialog->content.buttons.size() == 2 &&
          failureDialog->retryAction ==
              playback_media_actions::Action::SeparateAudio &&
          mediaTaskStatusModel(failedSeparation).text ==
              "Audio separation failed. F1: Details" &&
          mediaTaskStatusModel(failedSeparation).tone ==
              MediaTaskStatusTone::Error,
      "failed work must retain actionable details in a reopenable dialog");
  ok &= expect(!unsupportedAction,
               "surface-specific actions must remain outside processing");

  processing::Coordinator::Operations unavailableOperations;
  unavailableOperations.separateAudio =
      audio_separation::OperationBinding::unavailable(
          audio_separation::OperationAvailability::SetupRequired,
          "Install the certified native NVIDIA provider.");
  processing::Coordinator unavailableCoordinator(
      std::move(unavailableOperations));
  playback_media_processing::Actions unavailableActions(
      unavailableCoordinator);
  const auto unavailableContext =
      unavailableActions.contextForSource("movie.mp4");
  const auto unavailableSeparation = unavailableActions.execute(
      playback_media_actions::Action::SeparateAudio, "movie.mp4");
  ok &= expect(
      !unavailableContext.canGenerateSubtitles &&
          !unavailableContext.canSeparateAudio && unavailableSeparation &&
          !unavailableSeparation->accepted && unavailableSeparation->error &&
          unavailableSeparation->error->failure ==
              playback_media_processing::RequestFailure::BackendUnavailable &&
          unavailableSeparation->feedback ==
              "Audio separation could not start: Install the certified "
              "native NVIDIA provider. Source: "
              "\"movie.mp4\".",
      "availability and start diagnostics must derive from the same injected "
      "backend");

  processing::Coordinator::Operations completionQueueOperations;
  completionQueueOperations.analyzeMelody =
      [](const std::filesystem::path&, int, const std::filesystem::path&,
         const processing::Coordinator::ProgressReporter&,
         const processing::Coordinator::CancellationRequested&,
         const processing::Coordinator::CommitStarted& beginCommit,
         std::string*) { return beginCommit(); };
  completionQueueOperations.splitLoop =
      [](const std::filesystem::path&, const std::filesystem::path&,
         const std::filesystem::path&, const LoopSplitConfig&,
         LoopSplitResult* result,
         const processing::Coordinator::ProgressReporter&,
         const processing::Coordinator::CancellationRequested&,
         const processing::Coordinator::CommitStarted& beginCommit,
         std::string*) {
        if (result) result->hasStinger = true;
        return beginCommit();
      };
  processing::Coordinator completionQueueCoordinator(
      std::move(completionQueueOperations));
  const auto firstQueuedStart =
      completionQueueCoordinator.tryStartMelodyAnalysis(
          "queued.flac", 0, "queued.melody");
  const bool firstQueuedFinished = waitUntil(
      [&]() { return !completionQueueCoordinator.running(); });
  const auto secondQueuedStart = completionQueueCoordinator.tryStartLoopSplit(
      "next.flac", "next_stinger.wav", "next_loop.wav", {});
  const processing::PollResult queuedUpdate =
      completionQueueCoordinator.poll();
  const bool retainedFirstCompletion = std::any_of(
      queuedUpdate.completions.begin(), queuedUpdate.completions.end(),
      [](const processing::TaskCompletion& completion) {
        return completion.kind == processing::TaskKind::MelodyAnalysis &&
               completion.sourceFile == "queued.flac";
      });
  ok &= expect(firstQueuedStart.wasAccepted() && firstQueuedFinished &&
                   secondQueuedStart.wasAccepted() && retainedFirstCompletion,
               "a completed result must be queued without rejecting the next "
               "request before the UI polls");
  completionQueueCoordinator.shutdown();

  ok &= expect(!coordinator.tryStartMelodyAnalysis({}, 0, "clip.melody")
                    .wasAccepted() &&
                   !coordinator.running(),
               "invalid work must not change coordinator state");
  ok &= expect(coordinator.audioSeparationAvailableFor("clip.mp4") &&
                   coordinator.audioSeparationAvailableFor("clip.flac") &&
                   !coordinator.audioSeparationAvailableFor(
                       "clip.dialogue.flac") &&
                   !coordinator.audioSeparationAvailableFor("notes.txt"),
               "availability must be centralized and reject managed stems");

  ok &= expect(coordinator.tryStartMelodyAnalysis(
                   "clip.flac", 0, "clip.melody").wasAccepted() &&
                   wakeIsSignaled(coordinator) &&
                   waitUntil([&]() {
                     return melodyStarted.load(std::memory_order_acquire);
                   }),
               "melody analysis must start through the coordinator");
  const std::optional<processing::TaskActivity> melody =
      coordinator.activity();
  const std::optional<MediaTaskCardModel> melodyCard =
      presenter.activeCard();
  const auto busyLoop = coordinator.tryStartLoopSplit(
      "other.flac", "other_stinger.wav", "other_loop.wav", {});
  const auto busySeparation = playbackActions.execute(
      playback_media_actions::Action::SeparateAudio, "other.mp4");
  ok &= expect(melody &&
                   melody->kind == processing::TaskKind::MelodyAnalysis &&
                   melody->progress && *melody->progress == 0.4f &&
                   melody->cancellable &&
                   melodyCard && melodyCard->title == "Analyzing melody" &&
                   melodyCard->taskId == melody->id &&
                   melodyCard->cancellable &&
                   !busyLoop.wasAccepted() && busyLoop.error() &&
                   busyLoop.error()->failure ==
                       playback_media_processing::RequestFailure::Busy &&
                   busyLoop.error()->blockingOperation ==
                       playback_media_processing::Operation::MelodyAnalysis &&
                   busyLoop.error()->blockingSourceFile == "clip.flac" &&
                   busySeparation && !busySeparation->accepted &&
                   busySeparation->feedback ==
                       "Audio separation could not start: melody analysis is "
                       "already running for \"clip.flac\". Source: "
                       "\"other.mp4\".",
               "one generic activity must explain which task enforces mutual "
               "exclusion");
  releaseMelody.store(true, std::memory_order_release);
  const auto melodyCompletion = waitForCompletion(coordinator);
  const std::optional<MediaTaskStatusModel> melodyStatus =
      presenter.latestStatus();
  ok &= expect(melodyCompletion && melodyCompletion->succeeded() &&
                   melodyCompletion->id == melody->id &&
                   melodyStatus && melodyStatus->text ==
                       "Analyze: Saved clip.melody and clip.mid" &&
                   melodyStatus->tone == MediaTaskStatusTone::Success &&
                   !processing::completionForPlayback(*melodyCompletion),
               "melody completion must use the shared result contract");

  ok &= expect(coordinator.tryStartLoopSplit(
                   "loop.flac", "loop_stinger.wav", "loop_loop.wav", {})
                   .wasAccepted() &&
                   !coordinator.latestCompletion() &&
                   waitUntil([&]() {
                     return loopStarted.load(std::memory_order_acquire);
                   }),
               "starting new work must retire the previous footer result");
  const std::optional<MediaTaskCardModel> loopCard = presenter.activeCard();
  const std::optional<processing::TaskActivity> loopActivity =
      coordinator.activity();
  ok &= expect(loopCard && loopActivity && !loopCard->progress &&
                   loopCard->taskId == loopActivity->id &&
                   loopCard->cancellable &&
                   loopCard->title == "Splitting loop",
               "tasks without measurable progress must stay indeterminate");
  ok &= expect(
      !coordinator.cancelActive(melody->id) &&
          coordinator.cancelActive(loopActivity->id) &&
          !coordinator.cancelActive(loopActivity->id),
               "generic background work must accept cancellation once");
  const std::optional<MediaTaskCardModel> cancellingLoopCard =
      presenter.activeCard();
  ok &= expect(cancellingLoopCard &&
                   !cancellingLoopCard->cancellable &&
                   cancellingLoopCard->cancelling &&
                   cancellingLoopCard->title == "Cancelling loop split",
               "generic cancellation must reach the shared task card");
  const auto cancelledLoopCompletion = waitForCompletion(coordinator);
  ok &= expect(cancelledLoopCompletion &&
                   cancelledLoopCompletion->id == loopActivity->id &&
                   cancelledLoopCompletion->outcome ==
                       processing::TaskOutcome::Cancelled,
               "a cancelled generic worker must publish a cancelled result");

  loopStarted.store(false, std::memory_order_release);
  releaseLoop.store(true, std::memory_order_release);
  ok &= expect(coordinator.tryStartLoopSplit(
                   "loop.flac", "loop_stinger.wav", "loop_loop.wav", {})
                   .wasAccepted() &&
                   waitUntil([&]() {
                     return loopStarted.load(std::memory_order_acquire);
                   }),
               "a completed cancellation must not poison the next task");
  const auto loopCompletion = waitForCompletion(coordinator);
  ok &= expect(loopCompletion && loopCompletion->succeeded() &&
                   mediaTaskStatusModel(*loopCompletion).text ==
                       "Loop split: Saved loop_stinger.wav and loop_loop.wav",
               "loop splitting must project through the same completion model");

  const std::optional<playback_media_processing::ActionResult> subtitleStart =
      playbackActions.execute(
          playback_media_actions::Action::GenerateSubtitles, "movie.mp4");
  ok &= expect(subtitleStart && subtitleStart->accepted &&
                   subtitleStart->feedback ==
                       "Generating subtitles" &&
                   wakeIsSignaled(coordinator) &&
                   waitUntil([&]() {
                     return subtitlesStarted.load(std::memory_order_acquire);
                   }) &&
                   coordinator.subtitleGenerationRunningFor("movie.mp4"),
               "subtitle generation must retain source identity");
  const std::optional<processing::TaskActivity> subtitles =
      coordinator.activity();
  const playback_media_processing::SourceState subtitleSourceState =
      coordinator.sourceStateFor("movie.mp4");
  ok &= expect(subtitles && subtitles->cancellable && subtitles->progress &&
                   subtitleSourceState.backgroundTaskRunning &&
                   subtitleSourceState.subtitleGenerationRunning &&
                   !subtitleSourceState.audioSeparationRunning &&
                   mediaTaskCardModel(*subtitles).title ==
                       "Generating subtitles" &&
                   mediaTaskCardModel(*subtitles).detail ==
                       "Transcribing audio" &&
                   mediaTaskCardModel(*subtitles).taskId == subtitles->id &&
                   mediaTaskCardModel(*subtitles).cancellable,
               "backend phases must reach the generic task card");
  releaseSubtitles.store(true, std::memory_order_release);
  const auto subtitleCompletion = waitForCompletion(coordinator);
  const auto playbackSubtitleCompletion =
      subtitleCompletion
          ? processing::completionForPlayback(*subtitleCompletion)
          : std::nullopt;
  ok &= expect(subtitleCompletion && subtitleCompletion->succeeded() &&
                   subtitleCompletion->id == subtitles->id &&
                   subtitleCompletion->outputFile ==
                       std::filesystem::path("movie.transcript.srt") &&
                   playbackSubtitleCompletion &&
                   playbackSubtitleCompletion->operation ==
                       playback_media_processing::Operation::
                           SubtitleGeneration &&
                   playbackSubtitleCompletion->succeeded() &&
                   playback_session::mediaTaskFeedback(
                       *playbackSubtitleCompletion) ==
                       "Subtitles ready: movie.transcript.srt" &&
                   mediaTaskStatusModel(*subtitleCompletion).text ==
                       "Subtitles ready: movie.transcript.srt",
               "subtitle completion must retain its canonical sidecar");

  auto playbackPriority = coordinator.acquireInteractivePlayback();
  auto secondPlaybackPriority = coordinator.acquireInteractivePlayback();
  const bool playbackPriorityReadyBeforeWork =
      playbackPriority.ready() && secondPlaybackPriority.ready();
  const std::optional<playback_media_processing::ActionResult>
      separationStart =
      playbackActions.execute(playback_media_actions::Action::SeparateAudio,
                              "movie.mp4");
  ok &= expect(separationStart && separationStart->accepted &&
                   separationStart->feedback ==
                       "Separating audio" &&
                   wakeIsSignaled(coordinator) &&
                   !separationStarted.load(std::memory_order_acquire) &&
                   coordinator.audioSeparationRunningFor("movie.mp4"),
               "foreground playback policy must register a resumable task "
               "without entering its GPU backend");
  const std::optional<processing::TaskActivity> pausedSeparation =
      coordinator.activity();
  const std::optional<MediaTaskCardModel> pausedSeparationCard =
      presenter.activeCard();
  playbackPriority.reset();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const bool retainedBySecondLease =
      !separationStarted.load(std::memory_order_acquire);
  secondPlaybackPriority.reset();
  const bool separationStartedAfterRelease = waitUntil([&]() {
    return separationStarted.load(std::memory_order_acquire) &&
           separationCheckpoints.load(std::memory_order_acquire) > 0;
  });
  ok &= expect(playbackPriorityReadyBeforeWork && retainedBySecondLease &&
                   pausedSeparation &&
                   pausedSeparation->scheduling ==
                       processing::TaskSchedulingState::Suspended &&
                   pausedSeparation->phase ==
                       "Video playback has priority" &&
                   pausedSeparationCard &&
                   pausedSeparationCard->title ==
                       "Audio separation paused" &&
                   pausedSeparationCard->engineName ==
                       "Test GPU backend" &&
                   separationStartedAfterRelease,
               "resource priority must surface as a typed paused state and "
               "enter the backend only after playback yields");

  auto runningPlaybackPriority = coordinator.acquireInteractivePlayback();
  const bool runningSeparationSuspended = waitUntil([&]() {
    return runningPlaybackPriority.ready();
  });
  const int checkpointsBeforeResume =
      separationCheckpoints.load(std::memory_order_acquire);
  runningPlaybackPriority.reset();
  const bool runningSeparationResumed = waitUntil([&]() {
    return separationCheckpoints.load(std::memory_order_acquire) >
           checkpointsBeforeResume;
  });
  ok &= expect(runningSeparationSuspended && runningSeparationResumed,
               "a foreground lease must become ready only after active "
               "background work suspends and must resume it on release");
  ok &= expect(pausedSeparation &&
                   coordinator.cancelActive(pausedSeparation->id) &&
                   !coordinator.cancelActive(pausedSeparation->id) &&
                   waitUntil([&]() {
                     return separationCancellationObserved.load(
                         std::memory_order_acquire);
                   }),
               "resumed separation must remain cancellable through its "
               "owner");
  const std::optional<processing::TaskActivity> cancelling =
      coordinator.activity();
  const playback_media_processing::SourceState separationSourceState =
      coordinator.sourceStateFor("movie.mp4");
  ok &= expect(cancelling && cancelling->cancelling &&
                   !cancelling->cancellable &&
                   separationSourceState.backgroundTaskRunning &&
                   !separationSourceState.subtitleGenerationRunning &&
                   separationSourceState.audioSeparationRunning &&
                   mediaTaskCardModel(*cancelling).title ==
                       "Cancelling audio separation" &&
                   !mediaTaskCardModel(*cancelling).cancellable &&
                   mediaTaskCardModel(*cancelling).cancelling,
               "cancellation must remain an explicit generic activity state");
  releaseSeparation.store(true, std::memory_order_release);
  const auto separationCompletion = waitForCompletion(coordinator);
  const auto playbackSeparationCompletion =
      separationCompletion
          ? processing::completionForPlayback(*separationCompletion)
          : std::nullopt;
  ok &= expect(separationCompletion &&
                   separationCompletion->id == pausedSeparation->id &&
                   separationCompletion->outcome ==
                       processing::TaskOutcome::Cancelled &&
                   playbackSeparationCompletion &&
                   playbackSeparationCompletion->operation ==
                       playback_media_processing::Operation::AudioSeparation &&
                   playbackSeparationCompletion->outcome ==
                       playback_media_processing::Outcome::Cancelled &&
                   playback_session::mediaTaskFeedback(
                       *playbackSeparationCompletion) ==
                       "Audio separation cancelled." &&
                   mediaTaskStatusModel(*separationCompletion).text ==
                       "Audio separation cancelled." &&
                   mediaTaskStatusModel(*separationCompletion).tone ==
                       MediaTaskStatusTone::Neutral,
               "cancelled separation must not leak backend error text");

  const auto exportStamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path exportDirectory =
      std::filesystem::temp_directory_path() /
      ("radioify-coordinator-export-tests-" +
       std::to_string(exportStamp));
  std::filesystem::create_directories(exportDirectory);
  const std::filesystem::path exportVideo = exportDirectory / "movie.mp4";
  const std::filesystem::path indexedTranscript =
      exportDirectory / "movie.transcript.srt";
  std::ofstream(exportVideo, std::ios::binary).put('\0');
  std::ofstream(indexedTranscript, std::ios::binary)
      << "1\r\n00:00:00,000 --> 00:00:01,000\r\nSpeech\r\n";

  const auto audioExportStart = playbackActions.execute(
      playback_media_actions::Action::ExportAudio, exportVideo);
  const bool audioExportRunning = waitUntil([&]() {
    return audioExportStarted.load(std::memory_order_acquire);
  });
  const auto audioExportActivity = coordinator.activity();
  const auto audioExportSourceState = coordinator.sourceStateFor(exportVideo);
  releaseAudioExport.store(true, std::memory_order_release);
  const auto audioExportCompletion = waitForCompletion(coordinator);
  const auto playbackAudioExportCompletion =
      audioExportCompletion
          ? processing::completionForPlayback(*audioExportCompletion)
          : std::nullopt;
  ok &= expect(
      audioExportStart && audioExportStart->accepted &&
          audioExportStart->feedback == "Exporting audio" &&
          audioExportRunning && audioExportActivity &&
          audioExportActivity->kind == processing::TaskKind::AudioExport &&
          mediaTaskCardModel(*audioExportActivity).title ==
              "Exporting audio" &&
          audioExportSourceState.audioExportRunning &&
          audioExportSourceState.transcriptTextExportAvailable &&
          observedAudioExportOutput.filename() == "movie - audio.flac" &&
          audioExportCompletion && audioExportCompletion->succeeded() &&
          playbackAudioExportCompletion &&
          playbackAudioExportCompletion->operation ==
              playback_media_processing::Operation::AudioExport &&
          playback_session::mediaTaskFeedback(
              *playbackAudioExportCompletion) ==
              "Audio export ready: movie - audio.flac" &&
          mediaTaskStatusModel(*audioExportCompletion).text ==
              "Audio export ready: movie - audio.flac",
      "audio export must share naming, progress and playback feedback");

  const auto transcriptExportStart = playbackActions.execute(
      playback_media_actions::Action::ExportTranscriptText, exportVideo);
  const bool transcriptExportRunning = waitUntil([&]() {
    return transcriptExportStarted.load(std::memory_order_acquire);
  });
  const auto transcriptExportActivity = coordinator.activity();
  const auto transcriptExportSourceState =
      coordinator.sourceStateFor(exportVideo);
  const auto cancelTranscriptExport = playbackActions.execute(
      playback_media_actions::Action::CancelMediaExport, exportVideo);
  const bool transcriptCancellationReachedWorker = waitUntil([&]() {
    return transcriptExportCancellationObserved.load(
        std::memory_order_acquire);
  });
  const auto transcriptExportCompletion = waitForCompletion(coordinator);
  ok &= expect(
      transcriptExportStart && transcriptExportStart->accepted &&
          transcriptExportRunning && transcriptExportActivity &&
          transcriptExportActivity->kind ==
              processing::TaskKind::TranscriptTextExport &&
          mediaTaskCardModel(*transcriptExportActivity).title ==
              "Exporting transcript" &&
          transcriptExportSourceState.transcriptTextExportRunning &&
          observedTranscriptExportOutput.filename() ==
              "movie - transcript.txt" &&
          cancelTranscriptExport && cancelTranscriptExport->accepted &&
          transcriptCancellationReachedWorker && transcriptExportCompletion &&
          transcriptExportCompletion->outcome ==
              processing::TaskOutcome::Cancelled &&
          mediaTaskStatusModel(*transcriptExportCompletion).text ==
              "Transcript export cancelled.",
      "text export cancellation must use the shared asynchronous task owner");

  std::error_code exportCleanupError;
  std::filesystem::remove_all(exportDirectory, exportCleanupError);

  processing::ActionRequest unsupportedRequest;
  unsupportedRequest.action = playback_media_actions::Action::EditVideo;
  unsupportedRequest.sourceFile = "movie.mp4";
  ok &= expect(!applicationActions.execute(unsupportedRequest),
               "surface navigation actions must remain outside processing");

  tui_media_tasks::Controller taskController(coordinator,
                                             applicationActions);

  processing::ActionRequest trackedMelodyRequest;
  trackedMelodyRequest.action =
      playback_media_actions::Action::AnalyzeAudio;
  trackedMelodyRequest.sourceFile = "album.flac";
  trackedMelodyRequest.trackIndex = 7;
  melodyStarted.store(false, std::memory_order_release);
  releaseMelody.store(false, std::memory_order_release);
  const auto trackedMelodyStart =
      taskController.execute(trackedMelodyRequest);
  const bool trackedMelodyRunning = waitUntil([&]() {
    return melodyStarted.load(std::memory_order_acquire);
  });
  const auto trackedMelodyCard = taskController.snapshot().activeCard;
  releaseMelody.store(true, std::memory_order_release);
  const auto trackedMelodyCompletion = waitForCompletion(taskController);
  ok &= expect(
      trackedMelodyStart && trackedMelodyStart->accepted &&
          trackedMelodyStart->feedback == "Analyzing melody" &&
          trackedMelodyRunning && trackedMelodyCard &&
          trackedMelodyCard->title == "Analyzing melody" &&
          trackedMelodyCompletion && trackedMelodyCompletion->succeeded() &&
          observedMelodyTrackIndex == 7 &&
          observedMelodyOutput ==
              std::filesystem::path("album.flac.track007.melody"),
      "application actions must own tracked melody output naming");

  processing::ActionRequest invalidTrackRequest = trackedMelodyRequest;
  invalidTrackRequest.trackIndex = -1;
  const auto invalidTrackResult =
      taskController.execute(invalidTrackRequest);
  ok &= expect(invalidTrackResult && !invalidTrackResult->accepted &&
                   !coordinator.running(),
               "invalid track selections must be rejected before task start");

  processing::ActionRequest splitRequest;
  splitRequest.action = playback_media_actions::Action::SplitLoop;
  splitRequest.sourceFile = "concert.flac";
  splitRequest.trackIndex = 4;
  splitRequest.outputArgument = R"(D:\exports\named.flac)";
  splitRequest.loopSplitConfig.minConfidence = 0.73f;
  loopStarted.store(false, std::memory_order_release);
  releaseLoop.store(false, std::memory_order_release);
  const auto splitStart = taskController.execute(splitRequest);
  const bool splitRunning = waitUntil([&]() {
    return loopStarted.load(std::memory_order_acquire);
  });
  releaseLoop.store(true, std::memory_order_release);
  const auto splitCompletion = waitForCompletion(taskController);
  ok &= expect(
      splitStart && splitStart->accepted &&
          splitStart->feedback == "Splitting loop" && splitRunning &&
          splitCompletion &&
          splitCompletion->succeeded() && observedLoopTrackIndex == 4 &&
          observedStingerOutput ==
              std::filesystem::path(R"(D:\exports\named_stinger.wav)") &&
          observedLoopOutput ==
              std::filesystem::path(R"(D:\exports\named_loop.wav)") &&
          observedLoopConfidence == 0.73f,
      "application actions must own loop settings and output resolution");

  separationStarted.store(false, std::memory_order_release);
  separationCancellationObserved.store(false, std::memory_order_release);
  releaseSeparation.store(false, std::memory_order_release);
  processing::ActionRequest cancellableRequest;
  cancellableRequest.action =
      playback_media_actions::Action::SeparateAudio;
  cancellableRequest.sourceFile = "cancel.mp4";
  const auto cancellableStart = taskController.execute(cancellableRequest);
  const bool cancellableRunning = waitUntil([&]() {
    return separationStarted.load(std::memory_order_acquire);
  });
  const auto cancellableCard = taskController.snapshot().activeCard;
  const auto cancellationRequest =
      playback_media_processing::prepareCancellation(
          playback_media_actions::Action::CancelAudioSeparation,
          cancellableRequest.sourceFile,
          coordinator.sourceStateFor(cancellableRequest.sourceFile));
  auto wrongSourceCancellation = cancellationRequest;
  if (wrongSourceCancellation) {
    wrongSourceCancellation->sourceFile = "other.mp4";
  }
  auto wrongOperationCancellation = cancellationRequest;
  if (wrongOperationCancellation) {
    wrongOperationCancellation->operation =
        playback_media_processing::Operation::AudioExport;
  }
  auto wrongTaskCancellation = cancellationRequest;
  if (wrongTaskCancellation) {
    wrongTaskCancellation->taskId =
        processing::TaskId{wrongTaskCancellation->taskId.value + 1};
  }
  const auto matchedCancellation =
      cancellationRequest
          ? taskController.cancellationTarget(*cancellationRequest)
          : std::nullopt;
  const bool wrongSourceRejected =
      wrongSourceCancellation &&
      !taskController.cancellationTarget(*wrongSourceCancellation);
  const bool wrongOperationRejected =
      wrongOperationCancellation &&
      !taskController.cancellationTarget(*wrongOperationCancellation);
  const bool wrongTaskRejected =
      wrongTaskCancellation &&
      !taskController.cancellationTarget(*wrongTaskCancellation);
  const bool cancellationAccepted =
      cancellationRequest &&
      taskController.confirmCancellation(*cancellationRequest);
  const bool cancellationReachedWorker = waitUntil([&]() {
    return separationCancellationObserved.load(std::memory_order_acquire);
  });
  const auto cancellingCard = taskController.snapshot().activeCard;
  releaseSeparation.store(true, std::memory_order_release);
  const auto controllerCancellation = waitForCompletion(taskController);
  ok &= expect(
      cancellableStart && cancellableStart->accepted && cancellableRunning &&
          cancellableCard && matchedCancellation &&
          matchedCancellation->taskId == cancellableCard->taskId &&
          wrongSourceRejected && wrongOperationRejected && wrongTaskRejected &&
          cancellationAccepted && cancellationReachedWorker &&
          cancellingCard &&
          cancellingCard->title == "Cancelling audio separation" &&
          !cancellingCard->cancellable && cancellingCard->cancelling &&
          controllerCancellation &&
          controllerCancellation->id == cancellableCard->taskId &&
          controllerCancellation->outcome ==
              processing::TaskOutcome::Cancelled &&
          taskController.snapshot().latestStatus &&
          taskController.snapshot().latestStatus->text ==
              "Audio separation cancelled.",
      "the TUI task controller must own cancellation and stable presentation");

  std::atomic<bool> commitPreparationEntered{false};
  std::atomic<bool> allowCommitBarrier{false};
  std::atomic<bool> commitBarrierEntered{false};
  std::atomic<bool> releaseCommitBarrier{false};
  processing::Coordinator::Operations commitBarrierOperations;
  commitBarrierOperations.exportAudio =
      [&](const std::filesystem::path&, const std::filesystem::path&,
          const processing::Coordinator::ProgressReporter&,
           const processing::Coordinator::CancellationRequested& cancellation,
           const processing::Coordinator::CommitStarted& beginCommit,
           std::string*) {
        commitPreparationEntered.store(true, std::memory_order_release);
        while (!allowCommitBarrier.load(std::memory_order_acquire) &&
               !cancellation()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (cancellation() || !beginCommit()) return false;
        commitBarrierEntered.store(true, std::memory_order_release);
        while (!releaseCommitBarrier.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
      };
  processing::Coordinator commitBarrierCoordinator(
      std::move(commitBarrierOperations));
  processing::Actions commitBarrierActions(commitBarrierCoordinator);
  tui_media_tasks::Controller commitBarrierTaskController(
      commitBarrierCoordinator, commitBarrierActions);
  const auto commitBarrierStart =
      commitBarrierCoordinator.requestAudioExport("commit-barrier.mp4");
  const bool commitPreparationRunning = waitUntil([&]() {
    return commitPreparationEntered.load(std::memory_order_acquire);
  });
  commitBarrierTaskController.poll();
  const auto staleCancellableCard =
      commitBarrierTaskController.snapshot().activeCard;
  allowCommitBarrier.store(true, std::memory_order_release);
  const bool commitBarrierRunning = waitUntil([&]() {
    return commitBarrierEntered.load(std::memory_order_acquire);
  });
  const auto committingActivity = commitBarrierCoordinator.activity();
  const bool lateCancellationRejected =
      staleCancellableCard &&
      !commitBarrierTaskController.cancelActive(staleCancellableCard->taskId);
  const auto refreshedCommittingCard =
      commitBarrierTaskController.snapshot().activeCard;
  releaseCommitBarrier.store(true, std::memory_order_release);
  const auto commitBarrierCompletion =
      waitForCompletion(commitBarrierTaskController);
  ok &= expect(
      commitBarrierStart.wasAccepted() && commitPreparationRunning &&
          staleCancellableCard && staleCancellableCard->cancellable &&
          commitBarrierRunning &&
          committingActivity && !committingActivity->cancellable &&
          !committingActivity->cancelling &&
          committingActivity->phase == "Publishing output" &&
          lateCancellationRejected && refreshedCommittingCard &&
          !refreshedCommittingCard->cancellable &&
          !refreshedCommittingCard->cancelling &&
          refreshedCommittingCard->detail == "Publishing output" &&
          commitBarrierCompletion &&
          commitBarrierCompletion->succeeded(),
      "a stale Cancel must be rejected at the commit barrier and refresh the "
      "TUI to its non-cancellable publication state immediately");

  std::atomic<bool> shutdownCommitEntered{false};
  std::atomic<bool> releaseShutdownCommit{false};
  std::atomic<bool> cancellationLeakedPastCommit{false};
  processing::Coordinator::Operations shutdownCommitOperations;
  shutdownCommitOperations.exportAudio =
      [&](const std::filesystem::path&, const std::filesystem::path&,
          const processing::Coordinator::ProgressReporter&,
          const processing::Coordinator::CancellationRequested& cancellation,
          const processing::Coordinator::CommitStarted& beginCommit,
          std::string*) {
        if (cancellation() || !beginCommit()) return false;
        shutdownCommitEntered.store(true, std::memory_order_release);
        while (!releaseShutdownCommit.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool cancelledAfterCommit = cancellation();
        cancellationLeakedPastCommit.store(cancelledAfterCommit,
                                           std::memory_order_release);
        return !cancelledAfterCommit;
      };
  processing::Coordinator shutdownCommitCoordinator(
      std::move(shutdownCommitOperations));
  const auto shutdownCommitStart =
      shutdownCommitCoordinator.requestAudioExport("shutdown-commit.mp4");
  const bool shutdownCommitRunning = waitUntil([&]() {
    return shutdownCommitEntered.load(std::memory_order_acquire);
  });
  std::thread releaseShutdown([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    releaseShutdownCommit.store(true, std::memory_order_release);
  });
  shutdownCommitCoordinator.shutdown();
  releaseShutdown.join();
  const auto shutdownCommitCompletion =
      waitForCompletion(shutdownCommitCoordinator);
  ok &= expect(
      shutdownCommitStart.wasAccepted() && shutdownCommitRunning &&
          !cancellationLeakedPastCommit.load(std::memory_order_acquire) &&
          shutdownCommitCompletion && shutdownCommitCompletion->succeeded(),
      "shutdown must join an in-flight output commit without violating its "
      "point-of-no-return contract");

  std::atomic<int> retryRuns{0};
  processing::Coordinator::Operations retryOperations;
  retryOperations.exportAudio =
      [&](const std::filesystem::path&, const std::filesystem::path&,
          const processing::Coordinator::ProgressReporter&,
          const processing::Coordinator::CancellationRequested& cancellation,
          const processing::Coordinator::CommitStarted&,
          std::string* error) {
        const int run = retryRuns.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (run == 1) {
          if (error) *error = "Controlled first-attempt failure.";
          return false;
        }
        while (!cancellation()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
      };
  processing::Coordinator retryCoordinator(std::move(retryOperations));
  processing::Actions retryActions(retryCoordinator);
  tui_media_tasks::Controller retryController(retryCoordinator, retryActions);
  processing::ActionRequest retryRequest;
  retryRequest.action = playback_media_actions::Action::ExportAudio;
  retryRequest.sourceFile = "retry.mp4";
  const auto firstAttempt = retryController.execute(retryRequest);
  const auto firstFailure = waitForCompletion(retryController);
  const auto failureSnapshot = retryController.snapshot().latestFailure;
  const processing::TaskId staleFailure{
      failureSnapshot ? failureSnapshot->taskId.value + 1 : 1};
  const auto staleRetry = retryController.retry(staleFailure, retryRequest);
  const auto acceptedRetry = failureSnapshot
                                 ? retryController.retry(
                                       failureSnapshot->taskId, retryRequest)
                                 : std::nullopt;
  const bool retryStarted = waitUntil([&]() {
    return retryRuns.load(std::memory_order_acquire) == 2;
  });
  const auto retryCard = retryController.snapshot().activeCard;
  const bool retryCancelled =
      retryCard && retryController.cancelActive(retryCard->taskId);
  const auto retryCompletion = waitForCompletion(retryController);
  ok &= expect(firstAttempt && firstAttempt->accepted && firstFailure &&
                   firstFailure->outcome == processing::TaskOutcome::Failed &&
                   failureSnapshot && !staleRetry && acceptedRetry &&
                   acceptedRetry->accepted && retryStarted && retryCancelled &&
                   retryCompletion &&
                   retryCompletion->outcome ==
                       processing::TaskOutcome::Cancelled,
               "Retry must be accepted only for the exact latest failure "
               "identity and must start a new cancellable task");
  retryCoordinator.shutdown();

  coordinator.shutdown();
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
