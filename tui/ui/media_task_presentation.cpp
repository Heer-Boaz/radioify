#include "tui/ui/media_task_presentation.h"

#include <algorithm>

#include "app/media_processing_coordinator.h"
#include "core/runtime_helpers.h"

namespace {

std::string activityTitle(const media_processing::TaskActivity& activity) {
  using Kind = media_processing::TaskKind;
  switch (activity.kind) {
    case Kind::MelodyAnalysis:
      return "Analyzing melody";
    case Kind::LoopSplit:
      return "Splitting loop";
    case Kind::SubtitleGeneration:
      return activity.cancelling ? "Cancelling subtitles"
                                 : "Generating subtitles";
    case Kind::AudioSeparation:
      return activity.cancelling ? "Cancelling audio separation"
                                 : "Separating audio";
  }
  return "Processing media";
}

std::string completionText(
    const media_processing::TaskCompletion& completion) {
  using Kind = media_processing::TaskKind;
  using Outcome = media_processing::TaskOutcome;
  if (completion.outcome == Outcome::Cancelled) {
    switch (completion.kind) {
      case Kind::SubtitleGeneration:
        return "Subtitle generation cancelled.";
      case Kind::AudioSeparation:
        return "Audio separation cancelled.";
      case Kind::MelodyAnalysis:
        return "Melody analysis cancelled.";
      case Kind::LoopSplit:
        return "Loop split cancelled.";
    }
  }

  if (completion.outcome == Outcome::Failed) {
    if (!completion.detail.empty()) return completion.detail;
    switch (completion.kind) {
      case Kind::MelodyAnalysis:
        return "Melody analysis failed.";
      case Kind::LoopSplit:
        return "Loop split failed.";
      case Kind::SubtitleGeneration:
        return "Subtitle generation failed.";
      case Kind::AudioSeparation:
        return "Audio separation failed.";
    }
  }

  switch (completion.kind) {
    case Kind::MelodyAnalysis:
      return completion.detail.empty()
                 ? "Melody analysis complete."
                 : "Analyze: " + completion.detail;
    case Kind::LoopSplit:
      return completion.detail.empty()
                 ? "Loop split complete."
                 : "Loop split: " + completion.detail;
    case Kind::SubtitleGeneration: {
      const std::string filename =
          toUtf8String(completion.outputFile.filename());
      return filename.empty() ? "Subtitles ready."
                              : "Subtitles ready: " + filename;
    }
    case Kind::AudioSeparation:
      return "Audio stems ready: dialogue, music and effects.";
  }
  return {};
}

}  // namespace

MediaTaskCardModel mediaTaskCardModel(
    const media_processing::TaskActivity& activity) {
  MediaTaskCardModel model;
  model.title = activityTitle(activity);
  model.sourceName = activity.sourceFile.empty()
                         ? std::string("(unknown)")
                         : toUtf8String(activity.sourceFile.filename());
  model.detail = activity.phase;
  if (activity.progress) {
    model.progress = std::clamp(*activity.progress, 0.0f, 1.0f);
  }
  model.cancellable = activity.cancellable;
  return model;
}

MediaTaskStatusModel mediaTaskStatusModel(
    const media_processing::TaskCompletion& completion) {
  MediaTaskStatusModel model;
  model.text = completionText(completion);
  model.succeeded = completion.succeeded();
  return model;
}

std::optional<MediaTaskCardModel> MediaTaskPresenter::activeCard() const {
  const std::optional<media_processing::TaskActivity> activity =
      coordinator_.activity();
  return activity ? std::optional<MediaTaskCardModel>(
                        mediaTaskCardModel(*activity))
                  : std::nullopt;
}

std::optional<MediaTaskStatusModel> MediaTaskPresenter::latestStatus() const {
  const std::optional<media_processing::TaskCompletion> completion =
      coordinator_.latestCompletion();
  return completion ? std::optional<MediaTaskStatusModel>(
                          mediaTaskStatusModel(*completion))
                    : std::nullopt;
}
