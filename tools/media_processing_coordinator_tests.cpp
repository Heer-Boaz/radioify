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

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
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
  std::atomic<bool> separationCancellationObserved{false};
  std::atomic<bool> releaseSeparation{false};
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
          std::string*) {
        observedMelodyTrackIndex = trackIndex;
        observedMelodyOutput = outputFile;
        progress(0.4f, "Analyzing melody");
        melodyStarted.store(true, std::memory_order_release);
        while (!releaseMelody.load(std::memory_order_acquire) &&
               !cancellation()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return !cancellation();
      };
  operations.splitLoop =
      [&](const std::filesystem::path&,
          const std::filesystem::path& stingerOutput,
          const std::filesystem::path& loopOutput,
          const LoopSplitConfig& config,
          LoopSplitResult* result,
          const processing::Coordinator::ProgressReporter&,
          const processing::Coordinator::CancellationRequested& cancellation,
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
        return true;
      };
  operations.generateSubtitles =
      [&](const std::filesystem::path&, const std::filesystem::path&,
          const playback_video_transcript::GenerationJob::ProgressReporter&
              progress,
          const std::atomic<bool>*, std::string*) {
        progress(0.3f, "Transcribing audio");
        subtitlesStarted.store(true, std::memory_order_release);
        while (!releaseSubtitles.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
      };
  operations.separateAudio =
      [&](const std::filesystem::path&,
          const audio_separation::ArtifactPaths&,
          const audio_separation::Job::ProgressReporter& progress,
          const std::atomic<bool>* cancelRequested, std::string* error) {
        progress(0.2f, "Separating dialogue, music and effects on GPU");
        separationStarted.store(true, std::memory_order_release);
        while (!cancelRequested->load(std::memory_order_relaxed)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        separationCancellationObserved.store(true, std::memory_order_release);
        while (!releaseSeparation.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (error) *error = "Controlled cancellation.";
        return false;
      };
  operations.audioSeparationAvailable = true;
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
  ok &= expect(!presenter.activeCard() && !presenter.latestStatus(),
               "the presenter must not invent inactive task state");
  ok &= expect(!unsupportedAction,
               "surface-specific actions must remain outside processing");
  ok &= expect(!coordinator.tryStartMelodyAnalysis({}, 0, "clip.melody") &&
                   !coordinator.running(),
               "invalid work must not change coordinator state");
  ok &= expect(coordinator.audioSeparationAvailableFor("clip.mp4") &&
                   coordinator.audioSeparationAvailableFor("clip.flac") &&
                   !coordinator.audioSeparationAvailableFor(
                       "clip.dialogue.flac") &&
                   !coordinator.audioSeparationAvailableFor("notes.txt"),
               "availability must be centralized and reject managed stems");

  ok &= expect(coordinator.tryStartMelodyAnalysis(
                   "clip.flac", 0, "clip.melody") &&
                   wakeIsSignaled(coordinator) &&
                   waitUntil([&]() {
                     return melodyStarted.load(std::memory_order_acquire);
                   }),
               "melody analysis must start through the coordinator");
  const std::optional<processing::TaskActivity> melody =
      coordinator.activity();
  const std::optional<MediaTaskCardModel> melodyCard =
      presenter.activeCard();
  ok &= expect(melody &&
                   melody->kind == processing::TaskKind::MelodyAnalysis &&
                   melody->progress && *melody->progress == 0.4f &&
                   melody->cancellable &&
                   melodyCard && melodyCard->title == "Analyzing melody" &&
                   melodyCard->cancelAction &&
                   melodyCard->cancelAction->shortcut == "F8" &&
                   melodyCard->cancelAction->label == "Cancel" &&
                   !coordinator.tryStartLoopSplit(
                       "other.flac", "other_stinger.wav", "other_loop.wav",
                       {}),
               "one generic activity must enforce mutual exclusion");
  releaseMelody.store(true, std::memory_order_release);
  const auto melodyCompletion = waitForCompletion(coordinator);
  const std::optional<MediaTaskStatusModel> melodyStatus =
      presenter.latestStatus();
  ok &= expect(melodyCompletion && melodyCompletion->succeeded() &&
                   melodyStatus && melodyStatus->text ==
                       "Analyze: Saved clip.melody and clip.mid" &&
                   !processing::completionForPlayback(*melodyCompletion),
               "melody completion must use the shared result contract");

  ok &= expect(coordinator.tryStartLoopSplit(
                   "loop.flac", "loop_stinger.wav", "loop_loop.wav", {}) &&
                   !coordinator.latestCompletion() &&
                   waitUntil([&]() {
                     return loopStarted.load(std::memory_order_acquire);
                   }),
               "starting new work must retire the previous footer result");
  const std::optional<MediaTaskCardModel> loopCard = presenter.activeCard();
  ok &= expect(loopCard && !loopCard->progress &&
                   loopCard->cancelAction &&
                   loopCard->cancelAction->shortcut == "F8" &&
                   loopCard->title == "Splitting loop",
               "tasks without measurable progress must stay indeterminate");
  ok &= expect(coordinator.cancelActive() && !coordinator.cancelActive(),
               "generic background work must accept cancellation once");
  const std::optional<MediaTaskCardModel> cancellingLoopCard =
      presenter.activeCard();
  ok &= expect(cancellingLoopCard &&
                   !cancellingLoopCard->cancelAction &&
                   cancellingLoopCard->title == "Cancelling loop split",
               "generic cancellation must reach the shared task card");
  const auto cancelledLoopCompletion = waitForCompletion(coordinator);
  ok &= expect(cancelledLoopCompletion &&
                   cancelledLoopCompletion->outcome ==
                       processing::TaskOutcome::Cancelled,
               "a cancelled generic worker must publish a cancelled result");

  loopStarted.store(false, std::memory_order_release);
  releaseLoop.store(true, std::memory_order_release);
  ok &= expect(coordinator.tryStartLoopSplit(
                   "loop.flac", "loop_stinger.wav", "loop_loop.wav", {}) &&
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
                   mediaTaskCardModel(*subtitles).cancelAction &&
                   mediaTaskCardModel(*subtitles).cancelAction->shortcut ==
                       "F8",
               "backend phases must reach the generic task card");
  releaseSubtitles.store(true, std::memory_order_release);
  const auto subtitleCompletion = waitForCompletion(coordinator);
  const auto playbackSubtitleCompletion =
      subtitleCompletion
          ? processing::completionForPlayback(*subtitleCompletion)
          : std::nullopt;
  ok &= expect(subtitleCompletion && subtitleCompletion->succeeded() &&
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

  const std::optional<playback_media_processing::ActionResult>
      separationStart =
      playbackActions.execute(playback_media_actions::Action::SeparateAudio,
                              "movie.mp4");
  ok &= expect(separationStart && separationStart->accepted &&
                   separationStart->feedback ==
                       "Separating audio" &&
                   wakeIsSignaled(coordinator) &&
                   waitUntil([&]() {
                     return separationStarted.load(std::memory_order_acquire);
                   }) &&
                   coordinator.audioSeparationRunningFor("movie.mp4") &&
                   coordinator.cancelActive() &&
                   !coordinator.cancelActive() &&
                   waitUntil([&]() {
                     return separationCancellationObserved.load(
                         std::memory_order_acquire);
                   }),
               "F8 cancellation must dispatch through the active task owner");
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
                   !mediaTaskCardModel(*cancelling).cancelAction,
               "cancellation must remain an explicit generic activity state");
  releaseSeparation.store(true, std::memory_order_release);
  const auto separationCompletion = waitForCompletion(coordinator);
  const auto playbackSeparationCompletion =
      separationCompletion
          ? processing::completionForPlayback(*separationCompletion)
          : std::nullopt;
  ok &= expect(separationCompletion &&
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
                       "Audio separation cancelled.",
               "cancelled separation must not leak backend error text");

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
  const bool cancellationAccepted =
      taskController.cancelActive();
  const bool cancellationReachedWorker = waitUntil([&]() {
    return separationCancellationObserved.load(std::memory_order_acquire);
  });
  const auto cancellingCard = taskController.snapshot().activeCard;
  releaseSeparation.store(true, std::memory_order_release);
  const auto controllerCancellation = waitForCompletion(taskController);
  ok &= expect(
      cancellableStart && cancellableStart->accepted && cancellableRunning &&
          cancellationAccepted && cancellationReachedWorker &&
          cancellingCard &&
          cancellingCard->title == "Cancelling audio separation" &&
          !cancellingCard->cancelAction && controllerCancellation &&
          controllerCancellation->outcome ==
              processing::TaskOutcome::Cancelled &&
          taskController.snapshot().latestStatus &&
          taskController.snapshot().latestStatus->text ==
              "Audio separation cancelled.",
      "the TUI task controller must own cancellation and stable presentation");

  coordinator.shutdown();
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
