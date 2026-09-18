#include "playback/session/video_edit_workspace.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "core/runtime_helpers.h"
#include "core/wake_event.h"
#include "playback/video/analysis/scene_analysis_job.h"
#include "playback/video/edit/export.h"
#include "playback/video/edit/scene_suggestion_review.h"
#include "playback/video/edit/scene_suggestions.h"
#include "playback/video/edit/suggestion_preview.h"
#include "playback/video/edit/timeline.h"
#include "playback/video/player.h"
#include "playback/video/timeline_preview.h"
#include "playback/video/timeline_preview_model.h"
#include "playback/video/transcript/artifact.h"

namespace playback_session {
namespace {

struct CommandContext {
  int64_t sourceDurationUs = 0;
  int64_t positionUs = 0;
  int64_t sourcePositionUs = 0;
  int64_t frameDurationUs = 1;
  int64_t sequenceFrameDurationUs = 1;
  int videoStreamIndex = -1;
};

struct SelectedCut {
  int64_t outgoingEndUs = 0;
  int64_t incomingStartUs = 0;
};

}  // namespace

struct VideoEditWorkspace::Impl {
  Impl(std::filesystem::path path, Player& playbackPlayer, SubtitleManager& subtitles,
       playback_video_timeline_preview::HoverModel& preview,
       playback_video_timeline_preview::Provider& previewProvider)
      : sourcePath(std::move(path)),
        player(playbackPlayer),
        subtitles(subtitles),
        timelinePreview(preview),
        timelinePreviewProvider(previewProvider),
        wakeEvent(),
        exporter(wakeEvent.notifier()),
        sceneAnalysis(wakeEvent.notifier()) {}

  std::filesystem::path sourcePath;
  Player& player;
  SubtitleManager& subtitles;
  playback_video_timeline_preview::HoverModel& timelinePreview;
  playback_video_timeline_preview::Provider& timelinePreviewProvider;
  WakeEvent wakeEvent;
  playback_video_edit::Document document;
  playback_video_edit::Selection selection;
  std::optional<SelectedCut> selectedCut;
  bool active = false;
  playback_video_edit::Prompt prompt = playback_video_edit::Prompt::None;
  playback_video_edit::Exporter exporter;
  playback_video_analysis::SceneAnalysisJob sceneAnalysis;
  playback_video_edit::SceneSuggestionReview sceneSuggestionReview;
  playback_video_edit::SuggestionPreview suggestionPreview;

  struct AppliedExportCompletion {
    VideoEditExportCompletion outcome = VideoEditExportCompletion::None;
    bool exportedCurrentRevision = false;
    std::string message;
  };

  CommandContext commandContext() const {
    CommandContext context;
    context.sourceDurationUs = player.sourceDurationUs();
    const PlayerTimelineSnapshot timeline = player.timelineSnapshot();
    context.positionUs = std::max<int64_t>(0, timeline.positionUs);
    context.sourcePositionUs = std::clamp(
        timeline.sourcePositionUs, int64_t{0},
        std::max<int64_t>(0, context.sourceDurationUs));
    context.frameDurationUs =
        std::max<int64_t>(1, timeline.frameDurationUs);
    context.sequenceFrameDurationUs = std::max<int64_t>(
        1, timeline.nominalFrameDurationUs > 0
               ? timeline.nominalFrameDurationUs
               : timeline.frameDurationUs);
    context.videoStreamIndex = player.videoStreamIndex();
    return context;
  }

  std::optional<size_t> resolvedSelectedCutIndex() const {
    if (!selectedCut) return std::nullopt;
    const auto& ranges = document.timeline().keptRanges();
    for (size_t index = 0; index + 1 < ranges.size(); ++index) {
      if (ranges[index].endUs == selectedCut->outgoingEndUs &&
          ranges[index + 1].startUs == selectedCut->incomingStartUs) {
        return index;
      }
    }
    return std::nullopt;
  }

  bool canEnableMotionTransition(size_t cutIndex) const {
    const auto& ranges = document.timeline().keptRanges();
    auto transitions = document.timeline().cutTransitions();
    if (cutIndex + 1 >= ranges.size() || cutIndex >= transitions.size()) {
      return false;
    }
    transitions[cutIndex] =
        playback_video_edit::CutTransition::motionSmooth();
    return player.canRenderPlaybackComposition(ranges, transitions);
  }

  playback_video_edit::EditSnapshot editSnapshot() const {
    std::optional<int64_t> playheadTimelineUs;
    int64_t timecodeFrameDurationUs = 0;
    if (active) {
      const PlayerTimelineSnapshot timeline = player.timelineSnapshot();
      playheadTimelineUs = timeline.positionUs;
      timecodeFrameDurationUs = timeline.nominalFrameDurationUs;
    }
    playback_video_edit::EditSnapshot snapshot =
        playback_video_edit::buildSnapshot(
            document, selection, active, playheadTimelineUs,
            timecodeFrameDurationUs);
    if (active) {
      if (const auto cut = resolvedSelectedCutIndex()) {
        snapshot.selectedCutTransition =
            document.timeline().cutTransitions()[*cut];
        snapshot.canToggleSmoothCut =
            snapshot.selectedCutTransition->kind ==
                playback_video_edit::CutTransitionKind::MotionSmooth ||
            canEnableMotionTransition(*cut);
      }
    }
    const playback_video_analysis::JobSnapshot analysis =
        sceneAnalysis.snapshot();
    snapshot.sceneAnalysisProgress = analysis.progress;
    snapshot.sceneAnalysisPhase = analysis.phase;
    snapshot.sceneAnalysisError = analysis.error;
    snapshot.suggestionReview = sceneSuggestionReview.snapshot(
        analysis.suggestions, document.timeline(), active);
    snapshot.suggestionReview.previewing = suggestionPreview.active();
    switch (analysis.state) {
      case playback_video_analysis::JobState::Idle:
        snapshot.sceneAnalysisStatus =
            playback_video_edit::SceneAnalysisStatus::Idle;
        break;
      case playback_video_analysis::JobState::Running:
        snapshot.sceneAnalysisStatus =
            playback_video_edit::SceneAnalysisStatus::Running;
        break;
      case playback_video_analysis::JobState::Pausing:
        snapshot.sceneAnalysisStatus = playback_video_edit::SceneAnalysisStatus::Pausing;
        break;
      case playback_video_analysis::JobState::Succeeded:
        snapshot.sceneAnalysisStatus =
            playback_video_edit::SceneAnalysisStatus::Ready;
        break;
      case playback_video_analysis::JobState::Failed:
        snapshot.sceneAnalysisStatus =
            playback_video_edit::SceneAnalysisStatus::Failed;
        break;
      case playback_video_analysis::JobState::Cancelled:
        snapshot.sceneAnalysisStatus =
            playback_video_edit::SceneAnalysisStatus::Cancelled;
        break;
    }
    return snapshot;
  }

  playback_video_edit::ExportProgress exportProgressSnapshot() const {
    const playback_video_edit::ExportSnapshot job = exporter.snapshot();
    playback_video_edit::ExportProgress out;
    out.fraction = job.progress;
    if (job.running()) {
      out.status = playback_video_edit::ExportStatus::Running;
      out.targetsCurrentRevision =
          job.decisions == document.timeline().decisionList();
    } else if (job.state == playback_video_edit::ExportState::Failed &&
               job.decisions == document.timeline().decisionList()) {
      // A terminal failure remains actionable only while its exact edit
      // revision is still current. New edits must not inherit stale job state.
      out.status = playback_video_edit::ExportStatus::Failed;
    }
    return out;
  }

  void finishEditing() {
    prompt = playback_video_edit::Prompt::None;
    active = false;
    selection.clear();
    selectedCut.reset();
    sceneSuggestionReview.leaveEditor();
  }

  std::string sceneSuggestionDescription(
      const playback_video_analysis::EditProposal& suggestion) const {
    const playback_video_edit::SceneSuggestionKind kind =
        playback_video_edit::projectSceneSuggestionKind(suggestion.disposition);
    std::ostringstream description;
    description << playback_video_edit::sceneSuggestionKindLabel(kind)
                << ": " << suggestion.reason;
    return description.str();
  }

  VideoEditActionResult startSceneAnalysis(const CommandContext& context, bool restart = false) {
    VideoEditActionResult result{true, std::nullopt, {}};
    if (!sceneSuggestionReview.panelVisible()) {
      sceneSuggestionReview.togglePanel(sceneAnalysis.snapshot().suggestions, document.timeline());
      return result;
    }
    if (!restart && sceneAnalysis.snapshot().state == playback_video_analysis::JobState::Succeeded) {
      result.message = "Suggestions are ready; use Analyse again to replace them";
      return result;
    }
    playback_video_analysis::JobRequest request;
    request.sourcePath = sourcePath;
    request.videoStreamIndex = context.videoStreamIndex;
    request.durationUs = context.sourceDurationUs;
    request.forceReanalysis = restart;
    request.englishText = playback_video_analysis::selectEnglishTextEvidence(subtitles, sourcePath);
    if (sceneAnalysis.start(std::move(request))) {
      sceneSuggestionReview.beginAnalysis();
      result.message = "Finding edit suggestions; you can keep editing";
    } else if (sceneAnalysis.snapshot().busy()) {
      result.message = "Video editing review is already running";
    } else {
      result.message = "Could not start video editing review";
    }
    return result;
  }

  bool selectSceneSuggestionRange(
      const playback_video_analysis::EditProposal& suggestion,
      const CommandContext& context) {
    const int64_t startUs = std::clamp(
        suggestion.startUs, int64_t{0}, context.sourceDurationUs);
    const int64_t endUs = std::clamp(
        suggestion.endUs, startUs, context.sourceDurationUs);
    if (endUs <= startUs) return false;
    const auto position = positionForSource(
        startUs, playback_video_sequence::SourceBias::Forward);
    if (!position || !player.requestSeek(*position)) return false;
    selection.clear();
    selection.markIn(document.timeline(), startUs, context.frameDurationUs);
    const int64_t outFrameUs = std::max(startUs, endUs - context.frameDurationUs);
    selection.markOut(document.timeline(), outFrameUs, endUs, context.frameDurationUs);
    return true;
  }

  VideoEditActionResult navigateSceneSuggestion(
      const CommandContext& context,
      playback_video_edit::SceneSuggestionNavigation direction) {
    VideoEditActionResult result{true, std::nullopt, {}};
    const playback_video_analysis::JobSnapshot analysis =
        sceneAnalysis.snapshot();
    const auto* suggestion = sceneSuggestionReview.navigate(
        analysis.suggestions, document.timeline(), context.sourcePositionUs,
        direction);
    if (!suggestion) {
      result.message = "No segment suggestions are available";
      return result;
    }
    result.message = sceneSuggestionDescription(*suggestion);
    return result;
  }

  void updateTimelinePreview(
      const std::vector<playback_video_edit::SourceRange>& ranges) {
    if (!timelinePreview.setSequence(ranges)) return;
    timelinePreviewProvider.cancelBefore(timelinePreview.requestId());
  }

  std::optional<int64_t> positionForSource(
      int64_t sourceUs, playback_video_sequence::SourceBias bias) const {
    const playback_video_edit::Timeline& timeline = document.timeline();
    const auto sequence = playback_video_sequence::Timeline::create(
        timeline.sourceDurationUs(), timeline.keptRanges());
    if (!sequence) return std::nullopt;
    const auto point = sequence->pointForSource(sourceUs, bias);
    return point ? std::optional<int64_t>(point->presentationUs)
                 : std::nullopt;
  }

  std::optional<size_t> selectedCutIndex(
      const CommandContext& context) const {
    if (const auto selected = resolvedSelectedCutIndex()) return selected;
    const int64_t frameDurationUs = context.sequenceFrameDurationUs;
    const int64_t toleranceUs = std::max<int64_t>(
        50'000,
        frameDurationUs <= (std::numeric_limits<int64_t>::max)() / 2
            ? frameDurationUs * 2
            : frameDurationUs);
    return document.timeline().nearestCutIndex(context.positionUs,
                                               toleranceUs);
  }

  bool syncDocumentPreview(int64_t positionUs) {
    // Publish the EDL and its program playhead as one control transaction.
    const playback_video_edit::Timeline& timeline = document.timeline();
    if (timeline.isUnmodified()) {
      if (!player.clearPlaybackSequence(positionUs)) return false;
    } else if (!player.setPlaybackComposition(
                   timeline.keptRanges(), timeline.cutTransitions(),
                   positionUs)) {
      return false;
    }
    updateTimelinePreview(timeline.keptRanges());
    return true;
  }

  std::optional<AppliedExportCompletion> takeExportCompletion() {
    const std::optional<playback_video_edit::ExportSnapshot> completed =
        exporter.takeCompletion();
    if (!completed) return std::nullopt;
    exporter.consumeChanged();

    AppliedExportCompletion applied;
    switch (completed->state) {
      case playback_video_edit::ExportState::Succeeded:
        applied.exportedCurrentRevision =
            document.hasUnexportedChanges() &&
            completed->decisions == document.timeline().decisionList();
        document.markExported(completed->decisions);
        applied.outcome = VideoEditExportCompletion::Succeeded;
        applied.message =
            "Exported " +
            toUtf8String(completed->destinationPath.filename());
        break;
      case playback_video_edit::ExportState::Failed:
        applied.outcome = VideoEditExportCompletion::Failed;
        applied.message = completed->error.empty()
                              ? "Export failed"
                              : "Export failed: " + completed->error;
        break;
      case playback_video_edit::ExportState::Cancelled:
        applied.outcome = VideoEditExportCompletion::Cancelled;
        applied.message = "Export cancelled";
        break;
      case playback_video_edit::ExportState::Idle:
      case playback_video_edit::ExportState::Running:
        break;
    }
    return applied;
  }

  VideoEditActionResult startExport(const CommandContext& context) {
    VideoEditActionResult result;
    result.handled = true;
    const playback_video_edit::ExportSnapshot previous = exporter.snapshot();
    if (previous.running()) {
      result.message = "An export is already running";
      return result;
    }
    // A terminal result remains owned by the job until the workspace applies
    // it. This prevents a new export from overwriting an unobserved revision.
    const std::optional<AppliedExportCompletion> completed =
        takeExportCompletion();
    if (completed && completed->exportedCurrentRevision) {
      result.message = completed->message;
      return result;
    }
    const playback_video_edit::Timeline& timeline = document.timeline();
    if (timeline.sourceDurationUs() <= 0) {
      result.message = "Open the video editor and make an edit first";
      return result;
    }
    if (timeline.isUnmodified()) {
      result.message = "Make at least one trim or deletion before exporting";
      return result;
    }
    const std::filesystem::path destination =
        playback_video_edit::uniqueEditedOutputPath(sourcePath);
    if (destination.empty()) {
      result.message = "Could not select a safe output filename";
      return result;
    }

    playback_video_edit::ExportRequest request;
    request.sourcePath = sourcePath;
    request.destinationPath = destination;
    request.decisions = timeline.decisionList();
    request.videoStreamIndex = context.videoStreamIndex;
    if (!exporter.start(std::move(request))) {
      result.message = "Could not start edit export";
      return result;
    }
    result.exportStarted = true;
    result.message =
        "Export started: " +
        toUtf8String(destination.filename());
    return result;
  }

  VideoEditActionResult cancelExport() {
    if (exporter.cancel()) {
      return {true, std::nullopt, "Cancelling edit export..."};
    }
    return {true, std::nullopt, "No export is running"};
  }

  VideoEditActionResult finish() {
    const bool changesRetained = document.hasUnexportedChanges();
    finishEditing();
    return {true, std::nullopt,
            changesRetained ? "Edit mode closed; changes retained"
                            : "Edit mode closed"};
  }
};

VideoEditWorkspace::VideoEditWorkspace(
    std::filesystem::path sourcePath, Player& player, SubtitleManager& subtitles,
    playback_video_timeline_preview::HoverModel& timelinePreview,
    playback_video_timeline_preview::Provider& timelinePreviewProvider)
    : impl_(std::make_unique<Impl>(std::move(sourcePath), player, subtitles,
                                   timelinePreview,
                                   timelinePreviewProvider)) {}

VideoEditWorkspace::~VideoEditWorkspace() = default;

bool VideoEditWorkspace::active() const {
  return impl_ && impl_->active;
}

playback_video_edit::Prompt VideoEditWorkspace::prompt() const {
  return impl_ ? impl_->prompt : playback_video_edit::Prompt::None;
}

bool VideoEditWorkspace::hasUnexportedChanges() const {
  return impl_ && impl_->document.hasUnexportedChanges();
}

bool VideoEditWorkspace::needsExitConfirmation() const {
  if (!impl_) return false;
  const playback_video_edit::ExitContext context = exitContext();
  return context.hasUnexportedChanges || context.exportRunning;
}

playback_video_edit::ExitContext VideoEditWorkspace::exitContext() const {
  if (!impl_) return {};
  const playback_video_edit::ExportSnapshot job = impl_->exporter.snapshot();
  return {
      impl_->document.hasUnexportedChanges(),
      job.running(),
      job.running() &&
          job.decisions == impl_->document.timeline().decisionList(),
  };
}

VideoEditActionResult VideoEditWorkspace::execute(
    playback_video_edit::Command command) {
  VideoEditActionResult result;
  if (!impl_) return result;
  const bool promptCommand =
      command == playback_video_edit::Command::ConfirmPrompt ||
      command == playback_video_edit::Command::CancelPrompt;
  if (impl_->prompt == playback_video_edit::Prompt::None && promptCommand) {
    return result;
  }
  if (impl_->prompt != playback_video_edit::Prompt::None && !promptCommand) {
    return result;
  }
  if (!impl_->active && command != playback_video_edit::Command::Open &&
      command != playback_video_edit::Command::StartExport &&
      command != playback_video_edit::Command::CancelExport &&
      command != playback_video_edit::Command::RequestDiscard &&
      !promptCommand) {
    return result;
  }
  result.handled = true;
  const auto previousSuggestion = impl_->sceneSuggestionReview.selectedId();

  const CommandContext context = impl_->commandContext();
  const std::vector<playback_video_edit::SourceRange> previousRanges =
      impl_->document.timeline().keptRanges();
  bool timelineChanged = false;
  // History and reset retain the current program coordinate. Edit operations
  // below override this only when they define a new review point.
  int64_t nextPositionUs = context.positionUs;

  switch (command) {
    case playback_video_edit::Command::Open:
      if (impl_->active) {
        result.handled = false;
      } else if (context.sourceDurationUs > 0) {
        impl_->document.load(context.sourceDurationUs);
        impl_->selection.clear();
        impl_->selectedCut.reset();
        impl_->active = true;
        impl_->prompt = playback_video_edit::Prompt::None;
        result.message = "Video editor opened";
      } else {
        result.message = "Video editor requires a known duration";
      }
      break;
    case playback_video_edit::Command::Finish:
      result = impl_->finish();
      break;
    case playback_video_edit::Command::RequestClose:
      impl_->prompt = playback_video_edit::Prompt::LeaveEditMode;
      result.message.clear();
      break;
    case playback_video_edit::Command::RequestDiscard:
      if (impl_->exporter.snapshot().running()) {
        result.message = "Cancel the active export before discarding edits";
      } else if (!impl_->document.timeline().isUnmodified()) {
        impl_->prompt = playback_video_edit::Prompt::DiscardEdits;
        result.message.clear();
      } else {
        result.message = "There are no edits to discard";
      }
      break;
    case playback_video_edit::Command::ConfirmPrompt:
      if (impl_->prompt == playback_video_edit::Prompt::LeaveEditMode) {
        impl_->finishEditing();
        result.message = "Edit mode closed; edited preview retained";
      } else if (impl_->prompt == playback_video_edit::Prompt::DiscardEdits) {
        impl_->prompt = playback_video_edit::Prompt::None;
        timelineChanged = impl_->document.discardAllChanges();
        result.message = timelineChanged ? "Edits discarded"
                                         : "There are no edits to discard";
      } else if (impl_->prompt == playback_video_edit::Prompt::RestartAnalysis) {
        impl_->prompt = playback_video_edit::Prompt::None;
        result = impl_->startSceneAnalysis(context, true);
      } else {
        result.handled = false;
      }
      break;
    case playback_video_edit::Command::CancelPrompt:
      impl_->prompt = playback_video_edit::Prompt::None;
      result.message.clear();
      break;
    case playback_video_edit::Command::MarkIn:
      impl_->selection.markIn(impl_->document.timeline(),
                              context.sourcePositionUs,
                              context.frameDurationUs);
      result.message = "Selection start set";
      break;
    case playback_video_edit::Command::ToggleIn:
      if (impl_->selection.inSourceUs()) {
        impl_->selection.clear(playback_video_edit::EditBoundary::In);
        result.message = "Selection start cleared";
      } else {
        impl_->selection.markIn(impl_->document.timeline(),
                                context.sourcePositionUs,
                                context.frameDurationUs);
        result.message = "Selection start set";
      }
      break;
    case playback_video_edit::Command::ClearIn:
      result.message =
          impl_->selection.clear(playback_video_edit::EditBoundary::In)
              ? "Selection start cleared"
              : "No selection start to clear";
      break;
    case playback_video_edit::Command::MarkOut:
      {
        const int64_t duration =
            std::max<int64_t>(0, context.sourceDurationUs);
        const int64_t playhead =
            std::clamp(context.sourcePositionUs, int64_t{0}, duration);
        const int64_t frameDuration =
            std::max<int64_t>(1, context.frameDurationUs);
        const int64_t out = frameDuration >= duration - playhead
                                ? duration
                                : playhead + frameDuration;
        impl_->selection.markOut(impl_->document.timeline(), playhead, out,
                                 context.frameDurationUs);
      }
      result.message = "Selection end set";
      break;
    case playback_video_edit::Command::ToggleOut:
      if (impl_->selection.outSourceUs()) {
        impl_->selection.clear(playback_video_edit::EditBoundary::Out);
        result.message = "Selection end cleared";
      } else {
        const int64_t duration =
            std::max<int64_t>(0, context.sourceDurationUs);
        const int64_t playhead =
            std::clamp(context.sourcePositionUs, int64_t{0}, duration);
        const int64_t frameDuration =
            std::max<int64_t>(1, context.frameDurationUs);
        const int64_t out = frameDuration >= duration - playhead
                                ? duration
                                : playhead + frameDuration;
        impl_->selection.markOut(impl_->document.timeline(), playhead, out,
                                 context.frameDurationUs);
        result.message = "Selection end set";
      }
      break;
    case playback_video_edit::Command::ClearOut:
      result.message =
          impl_->selection.clear(playback_video_edit::EditBoundary::Out)
              ? "Selection end cleared"
              : "No selection end to clear";
      break;
    case playback_video_edit::Command::ClearInAndOut:
      result.message = impl_->selection.clear()
                           ? "Selection cleared"
                           : "No selection to clear";
      break;
    case playback_video_edit::Command::StartSceneAnalysis:
      result = impl_->startSceneAnalysis(context);
      break;
    case playback_video_edit::Command::RestartSceneAnalysis:
      if (impl_->sceneAnalysis.snapshot().state == playback_video_analysis::JobState::Succeeded)
        impl_->prompt = playback_video_edit::Prompt::RestartAnalysis;
      break;
    case playback_video_edit::Command::CancelSceneAnalysis:
      if (impl_->sceneAnalysis.cancel()) {
        result.message = "Pausing analysis...";
      } else if (impl_->sceneAnalysis.snapshot().busy()) {
        result.message = "Analysis is already pausing";
      } else {
        result.message = "Video editing review is not running";
      }
      break;
    case playback_video_edit::Command::ToggleSceneSuggestions: {
      const playback_video_analysis::JobSnapshot analysis =
          impl_->sceneAnalysis.snapshot();
      impl_->sceneSuggestionReview.togglePanel(analysis.suggestions, impl_->document.timeline());
      break;
    }
    case playback_video_edit::Command::CloseSceneSuggestions:
      impl_->sceneSuggestionReview.closePanel();
      break;
    case playback_video_edit::Command::StopScenePreview:
      if (impl_->suggestionPreview.stop()) result.playbackPaused = true;
      break;
    case playback_video_edit::Command::PreviewSceneSuggestion: {
      const auto analysis = impl_->sceneAnalysis.snapshot();
      const auto* suggestion = impl_->sceneSuggestionReview.selectedSuggestion(
          analysis.suggestions, impl_->document.timeline());
      if (!suggestion) {
        result.message = "Choose a suggestion to preview";
        break;
      }
      const auto projected = playback_video_edit::projectSceneSuggestion(
          *suggestion, impl_->document.timeline(), true);
      if (projected.spans.empty() ||
          !impl_->player.requestSeek(projected.spans.front().timelineStartUs)) {
        result.message = "Could not start this preview";
        break;
      }
      const auto timeline = impl_->player.timelineSnapshot();
      impl_->suggestionPreview.start(
          projected.spans.front().timelineStartUs, projected.spans.back().timelineEndUs,
          timeline.latestSeekRequestGeneration);
      result.playbackPaused = false;
      result.message = "Preview playing; pauses at the suggestion's end";
      break;
    }
    case playback_video_edit::Command::CycleSceneSuggestionFilter: {
      const playback_video_analysis::JobSnapshot analysis =
          impl_->sceneAnalysis.snapshot();
      const playback_video_edit::SceneSuggestionFilter filter =
          impl_->sceneSuggestionReview.cycleFilter(
              analysis.suggestions, impl_->document.timeline());
      const size_t filteredCount =
          impl_->sceneSuggestionReview.filteredCount(
              analysis.suggestions, impl_->document.timeline());
      result.message =
          std::string("Suggestion filter: ") +
          playback_video_edit::sceneSuggestionFilterLabel(filter) + " (" +
          std::to_string(filteredCount) + ")";
      break;
    }
    case playback_video_edit::Command::PreviousSceneSuggestion:
      result = impl_->navigateSceneSuggestion(
          context, playback_video_edit::SceneSuggestionNavigation::Previous);
      break;
    case playback_video_edit::Command::NextSceneSuggestion:
      result = impl_->navigateSceneSuggestion(
          context, playback_video_edit::SceneSuggestionNavigation::Next);
      break;
    case playback_video_edit::Command::SelectSceneSuggestion: {
      const playback_video_analysis::JobSnapshot analysis =
          impl_->sceneAnalysis.snapshot();
      const auto* suggestion =
          impl_->sceneSuggestionReview.selectedSuggestion(
              analysis.suggestions, impl_->document.timeline());
      if (!suggestion) {
        result.message = "Choose a segment suggestion first";
      } else if (!impl_->selectSceneSuggestionRange(*suggestion, context)) {
        result.message = "Could not select that suggested segment";
      } else {
        impl_->suggestionPreview.stop();
        result.playbackPaused = true;
        result.message = "Selected " +
                         impl_->sceneSuggestionDescription(*suggestion);
      }
      break;
    }
    case playback_video_edit::Command::DismissSceneSuggestion: {
      const playback_video_analysis::JobSnapshot analysis =
          impl_->sceneAnalysis.snapshot();
      if (impl_->sceneSuggestionReview.hideSelected(
              analysis.suggestions, impl_->document.timeline())) {
        result.message = "Suggestion hidden (Undo hide is available)";
      } else {
        result.message = "Choose a segment suggestion first";
      }
      break;
    }
    case playback_video_edit::Command::UndoDismissSceneSuggestion: {
      const playback_video_analysis::JobSnapshot analysis =
          impl_->sceneAnalysis.snapshot();
      const bool restored = impl_->sceneSuggestionReview
                                .undoHide(analysis.suggestions,
                                          impl_->document.timeline())
                                .has_value();
      result.message = restored ? "Hidden suggestion restored"
                                : "No hidden suggestion to restore";
      break;
    }
    case playback_video_edit::Command::RippleDelete:
      if (const auto remove = impl_->selection.range()) {
        // A ripple delete leaves one edit point at the range's former start.
        const std::optional<int64_t> editPositionUs =
            impl_->positionForSource(
                remove->startUs,
                playback_video_sequence::SourceBias::Forward);
        timelineChanged = impl_->document.rippleDelete(*remove);
        if (timelineChanged) {
          nextPositionUs = editPositionUs.value_or(nextPositionUs);
        }
        result.message = timelineChanged ? "Section removed"
                                         : "That section cannot be removed";
      } else {
        result.message = "Set the selection start and end first";
      }
      break;
    case playback_video_edit::Command::Trim:
      if (const auto keep = impl_->selection.range()) {
        timelineChanged = impl_->document.trimTo(*keep);
        // A trimmed program is reviewed from its new sequence origin.
        if (timelineChanged) nextPositionUs = 0;
        result.message = timelineChanged ? "Only the selected section kept"
                                         : "That selection changes nothing";
      } else {
        result.message = "Set the selection start and end first";
      }
      break;
    case playback_video_edit::Command::ToggleSmoothCut:
      if (const auto cut = impl_->selectedCutIndex(context)) {
        const playback_video_edit::CutTransition current =
            impl_->document.timeline().cutTransitions()[*cut];
        const bool enable =
            current.kind !=
            playback_video_edit::CutTransitionKind::MotionSmooth;
        const MotionCompositionSupport support =
            impl_->player.motionCompositionSupport();
        if (enable && support == MotionCompositionSupport::InterlacedSource) {
          result.message = "Smooth cut is unavailable for interlaced video";
        } else if (enable &&
                   support == MotionCompositionSupport::PreviewUnavailable) {
          result.message = "Smooth cut preview is unavailable";
        } else if (enable && !impl_->canEnableMotionTransition(*cut)) {
          result.message = "Smooth cut needs longer adjacent clips";
        } else {
          timelineChanged = impl_->document.setCutTransition(
              *cut,
              enable ? playback_video_edit::CutTransition::motionSmooth()
                     : playback_video_edit::CutTransition::hard());
          result.message = timelineChanged
                               ? (enable ? "Smooth cut set (4f)"
                                         : "Hard cut restored")
                               : "Cut transition is unchanged";
        }
      } else {
        result.message = "Move the playhead to a cut first";
      }
      break;
    case playback_video_edit::Command::Undo:
      timelineChanged = impl_->document.undo();
      result.message = timelineChanged ? "Edit undone" : "Nothing to undo";
      break;
    case playback_video_edit::Command::Redo:
      timelineChanged = impl_->document.redo();
      result.message = timelineChanged ? "Edit redone" : "Nothing to redo";
      break;
    case playback_video_edit::Command::Reset:
      timelineChanged = impl_->document.resetEdits();
      result.message = timelineChanged ? "All edits reset"
                                       : "Sequence is unchanged";
      break;
    case playback_video_edit::Command::StartExport:
      result = impl_->startExport(context);
      break;
    case playback_video_edit::Command::CancelExport:
      result = impl_->cancelExport();
      break;
  }

  bool projectionAccepted = true;
  if (timelineChanged) {
    const bool playbackSequenceChanged =
        previousRanges != impl_->document.timeline().keptRanges();
    if (playbackSequenceChanged) {
      impl_->selection.clear();
      projectionAccepted = impl_->syncDocumentPreview(nextPositionUs);
    } else {
      const playback_video_edit::Timeline& timeline =
          impl_->document.timeline();
      projectionAccepted = impl_->player.updatePlaybackComposition(
          timeline.keptRanges(), timeline.cutTransitions());
    }
    const playback_video_analysis::JobSnapshot analysis =
        impl_->sceneAnalysis.snapshot();
    impl_->sceneSuggestionReview.reconcile(
        analysis.suggestions, impl_->document.timeline());
  }

  if (!projectionAccepted) {
    result.message = "Timeline changed; preview unavailable";
  }

  // Task progress and export commands do not interrupt an audition. Its
  // lifetime ends when its document, focus, or review surface changes.
  if (impl_->suggestionPreview.active() &&
      (timelineChanged || !impl_->active || !impl_->sceneSuggestionReview.panelVisible() ||
       impl_->prompt != playback_video_edit::Prompt::None ||
       previousSuggestion != impl_->sceneSuggestionReview.selectedId())) {
    impl_->suggestionPreview.stop();
    result.playbackPaused = true;
  }
  if (result.handled && command == playback_video_edit::Command::Open && impl_->active)
    result.playbackPaused = true;
  return result;
}

VideoEditActionResult VideoEditWorkspace::focusSceneSuggestion(uint64_t id) {
  VideoEditActionResult result;
  if (!impl_ || !impl_->active || impl_->prompt != playback_video_edit::Prompt::None ||
      !impl_->sceneSuggestionReview.panelVisible()) return result;
  const auto previous = impl_->sceneSuggestionReview.selectedId();
  result.handled = impl_->sceneSuggestionReview.select(
      id, impl_->sceneAnalysis.snapshot().suggestions, impl_->document.timeline());
  if (result.handled && previous != impl_->sceneSuggestionReview.selectedId() &&
      impl_->suggestionPreview.stop()) result.playbackPaused = true;
  return result;
}

bool VideoEditWorkspace::scrollSuggestions(int offset) {
  return impl_ && impl_->active && impl_->prompt == playback_video_edit::Prompt::None &&
         impl_->sceneSuggestionReview.scrollTo(offset);
}

VideoEditActionResult VideoEditWorkspace::navigateBack() {
  VideoEditActionResult result;
  if (!impl_) return result;
  if (impl_->prompt != playback_video_edit::Prompt::None) {
    return execute(playback_video_edit::Command::CancelPrompt);
  }
  if (!impl_->active) return result;
  if (impl_->suggestionPreview.active())
    return execute(playback_video_edit::Command::StopScenePreview);
  if (impl_->sceneSuggestionReview.panelVisible())
    return execute(playback_video_edit::Command::CloseSceneSuggestions);
  if (impl_->selection.hasMarks()) {
    return execute(playback_video_edit::Command::ClearInAndOut);
  }
  return execute(playback_video_edit::Command::RequestClose);
}

bool VideoEditWorkspace::moveBoundary(
    playback_video_edit::EditBoundary boundary, int64_t timelineUs) {
  if (!impl_ || !impl_->active) return false;
  const int64_t minimumDurationUs = std::max<int64_t>(
      1, impl_->player.timelineSnapshot().frameDurationUs);
  if (!impl_->selection.moveBoundary(impl_->document.timeline(), boundary,
                                     timelineUs, minimumDurationUs)) {
    return false;
  }
  return true;
}

VideoEditPollResult VideoEditWorkspace::poll() {
  VideoEditPollResult result;
  if (!impl_) return result;
  result.changed = impl_->wakeEvent.consume();
  if (impl_->suggestionPreview.active()) {
    const auto timeline = impl_->player.timelineSnapshot();
    const auto update = impl_->suggestionPreview.observe(timeline.positionUs,
        timeline.latestSeekRequestGeneration, timeline.handledSeekRequestGeneration,
        timeline.seekPending(), impl_->player.isEnded());
    if (update != playback_video_edit::SuggestionPreview::Update::None) {
      result.changed = true;
      if (update == playback_video_edit::SuggestionPreview::Update::EndReached)
        result.playbackPaused = true;
    }
  }
  const auto appendMessage = [&](const std::string& message) {
    if (message.empty()) return;
    if (!result.message.empty()) result.message += " | ";
    result.message += message;
  };

  if (impl_->exporter.consumeChanged()) {
    result.changed = true;
  }
  if (const auto completedExport = impl_->takeExportCompletion()) {
    result.changed = true;
    result.completion = completedExport->outcome;
    appendMessage(completedExport->message);
  }

  if (impl_->sceneAnalysis.consumeChanged()) {
    result.changed = true;
  }
  if (const auto completedAnalysis =
          impl_->sceneAnalysis.takeCompletion()) {
    result.changed = true;
    switch (completedAnalysis->state) {
      case playback_video_analysis::JobState::Succeeded: {
        const CommandContext context = impl_->commandContext();
        if (impl_->sceneSuggestionReview.panelVisible())
          impl_->sceneSuggestionReview.showCurrentOrFirst(
              completedAnalysis->suggestions, impl_->document.timeline(), context.sourcePositionUs);
        std::string message =
            "Edit suggestions ready: " +
            std::to_string(completedAnalysis->suggestions.size()) +
            " suggestions";
        message += ". Open Edit suggestions to review them; your edits are unchanged.";
        appendMessage(message);
        break;
      }
      case playback_video_analysis::JobState::Failed:
        appendMessage("Video editing review failed: " +
                      completedAnalysis->error);
        break;
      case playback_video_analysis::JobState::Cancelled:
        appendMessage("Analysis paused; progress saved");
        break;
      default:
        break;
    }
  }
  return result;
}

std::vector<NativeWaitHandle> VideoEditWorkspace::activityWaitHandles() const {
  std::vector<NativeWaitHandle> handles;
  if (!impl_) return handles;
  handles.reserve(3);
  const auto append = [&](NativeWaitHandle handle) {
    if (handle) handles.push_back(handle);
  };
  append(impl_->wakeEvent.nativeWaitHandle());
  append(impl_->sceneAnalysis.workerWaitHandle());
  append(impl_->exporter.workerWaitHandle());
  return handles;
}

std::vector<NativeWaitHandle> VideoEditWorkspace::stopWaitHandles() const {
  std::vector<NativeWaitHandle> handles;
  if (!impl_) return handles;
  handles.reserve(2);
  if (!impl_->sceneAnalysis.stopReady()) {
    handles.push_back(impl_->sceneAnalysis.workerWaitHandle());
  }
  if (!impl_->exporter.stopReady()) {
    handles.push_back(impl_->exporter.workerWaitHandle());
  }
  return handles;
}

bool VideoEditWorkspace::selectCutAt(int64_t timelineUs,
                                     int64_t toleranceUs) {
  if (!impl_ || !impl_->active) return false;
  const auto cut = impl_->document.timeline().nearestCutIndex(
      timelineUs, std::max<int64_t>(0, toleranceUs));
  if (!cut) {
    impl_->selectedCut.reset();
    return false;
  }
  const auto& ranges = impl_->document.timeline().keptRanges();
  impl_->selectedCut = SelectedCut{ranges[*cut].endUs,
                                  ranges[*cut + 1].startUs};
  return true;
}

bool VideoEditWorkspace::selectSceneSuggestionAt(int64_t timelineUs,
                                                 int64_t toleranceUs) {
  if (!impl_ || !impl_->active) return false;
  const playback_video_edit::EditSnapshot snapshot = impl_->editSnapshot();
  const std::optional<uint64_t> suggestion =
      playback_video_edit::sceneSuggestionAtTimeline(
          snapshot.suggestionReview.suggestions, timelineUs,
          std::max<int64_t>(0, toleranceUs));
  if (!suggestion) {
    impl_->sceneSuggestionReview.clearSelection();
    return false;
  }
  const playback_video_analysis::JobSnapshot analysis =
      impl_->sceneAnalysis.snapshot();
  return impl_->sceneSuggestionReview.select(
      *suggestion, analysis.suggestions, impl_->document.timeline());
}

void VideoEditWorkspace::clearCutSelection() {
  if (impl_) impl_->selectedCut.reset();
}

void VideoEditWorkspace::requestStop() {
  if (!impl_) return;
  impl_->sceneAnalysis.requestStop();
  impl_->exporter.requestStop();
}

bool VideoEditWorkspace::stopReady() const {
  return !impl_ ||
         (impl_->sceneAnalysis.stopReady() && impl_->exporter.stopReady());
}

bool VideoEditWorkspace::finishStop() {
  if (!impl_ || !stopReady()) return !impl_;
  return impl_->sceneAnalysis.finishStop() && impl_->exporter.finishStop();
}

void VideoEditWorkspace::stop() {
  if (!impl_) return;
  requestStop();
  impl_->sceneAnalysis.stop();
  impl_->exporter.stop();
}

playback_video_edit::EditSnapshot VideoEditWorkspace::edit() const {
  return impl_ ? impl_->editSnapshot()
               : playback_video_edit::EditSnapshot{};
}

playback_video_edit::ExportProgress
VideoEditWorkspace::exportProgress() const {
  return impl_ ? impl_->exportProgressSnapshot()
               : playback_video_edit::ExportProgress{};
}

}  // namespace playback_session
