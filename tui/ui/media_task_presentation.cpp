#include "tui/ui/media_task_presentation.h"

#include <algorithm>

#include "app/media_processing_coordinator.h"
#include "core/runtime_helpers.h"
#include "tui/shell_shortcuts.h"

namespace {

std::string activityTitle(const media_processing::TaskActivity& activity) {
  using Kind = media_processing::TaskKind;
  if (activity.kind == Kind::AudioSeparation) {
    switch (activity.scheduling) {
      case media_processing::TaskSchedulingState::Running:
        break;
      case media_processing::TaskSchedulingState::Suspending:
        return "Pausing audio separation";
      case media_processing::TaskSchedulingState::Suspended:
        return "Audio separation paused";
    }
  }
  switch (activity.kind) {
    case Kind::MelodyAnalysis:
      return activity.cancelling ? "Cancelling melody analysis"
                                 : "Analyzing melody";
    case Kind::LoopSplit:
      return activity.cancelling ? "Cancelling loop split"
                                 : "Splitting loop";
    case Kind::SubtitleGeneration:
      return activity.cancelling ? "Cancelling subtitles"
                                 : "Generating subtitles";
    case Kind::AudioSeparation:
      return activity.cancelling ? "Cancelling audio separation"
                                 : "Separating audio";
    case Kind::AudioExport:
      return activity.cancelling ? "Cancelling audio export"
                                 : "Exporting audio";
    case Kind::TranscriptTextExport:
      return activity.cancelling ? "Cancelling transcript export"
                                 : "Exporting transcript";
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
      case Kind::AudioExport:
        return "Audio export cancelled.";
      case Kind::TranscriptTextExport:
        return "Transcript export cancelled.";
    }
  }

  if (completion.outcome == Outcome::Failed) {
    std::string text;
    switch (completion.kind) {
      case Kind::MelodyAnalysis:
        text = "Melody analysis failed.";
        break;
      case Kind::LoopSplit:
        text = "Loop split failed.";
        break;
      case Kind::SubtitleGeneration:
        text = "Subtitle generation failed.";
        break;
      case Kind::AudioSeparation:
        text = "Audio separation failed.";
        break;
      case Kind::AudioExport:
        text = "Audio export failed.";
        break;
      case Kind::TranscriptTextExport:
        text = "Transcript export failed.";
        break;
    }
    const std::string shortcut(tui_shell_shortcuts::label(
        tui_shell_shortcuts::Action::ToggleCommandPalette));
    return shortcut.empty() ? text + " Open Commands for details."
                            : text + " " + shortcut + ": Details";
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
    case Kind::AudioExport: {
      const std::string filename =
          toUtf8String(completion.outputFile.filename());
      return filename.empty() ? "Audio export ready."
                              : "Audio export ready: " + filename;
    }
    case Kind::TranscriptTextExport: {
      const std::string filename =
          toUtf8String(completion.outputFile.filename());
      return filename.empty() ? "Transcript export ready."
                              : "Transcript export ready: " + filename;
    }
  }
  return {};
}

std::string failureTitle(media_processing::TaskKind kind) {
  using Kind = media_processing::TaskKind;
  switch (kind) {
    case Kind::MelodyAnalysis:
      return "Melody analysis failed";
    case Kind::LoopSplit:
      return "Loop split failed";
    case Kind::SubtitleGeneration:
      return "Subtitle generation failed";
    case Kind::AudioSeparation:
      return "Audio separation failed";
    case Kind::AudioExport:
      return "Audio export failed";
    case Kind::TranscriptTextExport:
      return "Transcript export failed";
  }
  return "Media processing failed";
}

std::string failureSummary(media_processing::TaskKind kind) {
  using Kind = media_processing::TaskKind;
  switch (kind) {
    case Kind::MelodyAnalysis:
      return "Radioify could not analyze the selected audio.";
    case Kind::LoopSplit:
      return "Radioify could not split the selected loop.";
    case Kind::SubtitleGeneration:
      return "Radioify could not generate subtitles for this file.";
    case Kind::AudioSeparation:
      return "Radioify could not separate this file into audio stems.";
    case Kind::AudioExport:
      return "Radioify could not export audio from this file.";
    case Kind::TranscriptTextExport:
      return "Radioify could not export this transcript.";
  }
  return "Radioify could not finish the media-processing task.";
}

std::optional<playback_media_actions::Action> retryAction(
    media_processing::TaskKind kind) {
  using Kind = media_processing::TaskKind;
  using Action = playback_media_actions::Action;
  switch (kind) {
    case Kind::SubtitleGeneration:
      return Action::GenerateSubtitles;
    case Kind::AudioSeparation:
      return Action::SeparateAudio;
    case Kind::AudioExport:
      return Action::ExportAudio;
    case Kind::TranscriptTextExport:
      return Action::ExportTranscriptText;
    case Kind::MelodyAnalysis:
    case Kind::LoopSplit:
      return std::nullopt;
  }
  return std::nullopt;
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
  if (activity.cancellable) {
    const std::string shortcut(tui_shell_shortcuts::label(
        tui_shell_shortcuts::Action::ToggleCommandPalette));
    model.actionHint = shortcut.empty()
                           ? "Open Commands to cancel"
                           : shortcut + ": Task actions";
  }
  return model;
}

MediaTaskStatusModel mediaTaskStatusModel(
    const media_processing::TaskCompletion& completion) {
  MediaTaskStatusModel model;
  model.text = completionText(completion);
  model.succeeded = completion.succeeded();
  return model;
}

std::optional<MediaTaskFailureDialogModel> mediaTaskFailureDialogModel(
    const media_processing::TaskCompletion& completion) {
  if (completion.outcome != media_processing::TaskOutcome::Failed) {
    return std::nullopt;
  }

  MediaTaskFailureDialogModel model;
  model.sourceFile = completion.sourceFile;
  model.retryAction = retryAction(completion.kind);
  model.content.title = failureTitle(completion.kind);
  model.content.text.push_back(
      {failureSummary(completion.kind), tui_dialog::TextTone::Error});
  model.content.text.push_back(
      {completion.detail.empty()
           ? "The processing backend did not provide an error description."
           : "Reason: " + completion.detail,
       tui_dialog::TextTone::Normal});
  if (!completion.sourceFile.empty()) {
    model.content.text.push_back(
        {"Source: " + toUtf8String(completion.sourceFile),
         tui_dialog::TextTone::Secondary});
  }
  if (!completion.diagnosticLog.empty()) {
    model.content.text.push_back(
        {"Diagnostics: " + toUtf8String(completion.diagnosticLog),
         tui_dialog::TextTone::Secondary});
  }
  model.content.buttons.push_back({kMediaTaskDialogClose, "Close"});
  if (model.retryAction) {
    model.content.buttons.push_back({kMediaTaskDialogRetry, "Retry"});
  }
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

std::optional<MediaTaskFailureDialogModel>
MediaTaskPresenter::latestFailure() const {
  const std::optional<media_processing::TaskCompletion> completion =
      coordinator_.latestCompletion();
  return completion ? mediaTaskFailureDialogModel(*completion)
                    : std::nullopt;
}
