#include "playback/session/video_edit_workspace.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/runtime_helpers.h"
#include "playback/video/analysis/scene_analysis_job.h"
#include "playback/video/edit/export.h"
#include "playback/video/edit/scene_suggestions.h"
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
  Impl(std::filesystem::path path, Player& playbackPlayer,
       playback_video_timeline_preview::HoverModel& preview,
       playback_video_timeline_preview::Provider& previewProvider)
      : sourcePath(std::move(path)),
        player(playbackPlayer),
        timelinePreview(preview),
        timelinePreviewProvider(previewProvider) {}

  std::filesystem::path sourcePath;
  Player& player;
  playback_video_timeline_preview::HoverModel& timelinePreview;
  playback_video_timeline_preview::Provider& timelinePreviewProvider;
  playback_video_edit::Document document;
  playback_video_edit::Selection selection;
  std::optional<SelectedCut> selectedCut;
  bool active = false;
  playback_video_edit::Prompt prompt = playback_video_edit::Prompt::None;
  playback_video_edit::Exporter exporter;
  playback_video_edit::ExportState observedExportState =
      playback_video_edit::ExportState::Idle;
  playback_video_analysis::SceneAnalysisJob sceneAnalysis;
  playback_video_analysis::JobState observedSceneAnalysisState =
      playback_video_analysis::JobState::Idle;
  uint64_t observedSceneAnalysisGeneration = 0;
  std::optional<uint64_t> selectedSceneSuggestionId;
  bool sceneSuggestionsPanelVisible = false;
  playback_video_edit::SceneSuggestionFilter sceneSuggestionFilter =
      playback_video_edit::SceneSuggestionFilter::All;
  std::unordered_set<uint64_t> dismissedSceneSuggestionIds;
  std::vector<uint64_t> dismissedSceneSuggestionHistory;

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
    snapshot.sceneAnalysisUsedTranscript = analysis.usedIndexedTranscript;
    snapshot.sceneSuggestionsPanelVisible =
        active && sceneSuggestionsPanelVisible;
    snapshot.sceneSuggestionFilter = sceneSuggestionFilter;
    snapshot.canUndoSceneSuggestionDismissal =
        !dismissedSceneSuggestionHistory.empty();
    switch (analysis.state) {
      case playback_video_analysis::JobState::Idle:
        snapshot.sceneAnalysisStatus =
            playback_video_edit::SceneAnalysisStatus::Idle;
        break;
      case playback_video_analysis::JobState::Running:
        snapshot.sceneAnalysisStatus =
            playback_video_edit::SceneAnalysisStatus::Running;
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
    if (analysis.state == playback_video_analysis::JobState::Succeeded) {
      const auto allVisible = visibleSceneSuggestions(
          analysis, playback_video_edit::SceneSuggestionFilter::All);
      const auto filtered =
          visibleSceneSuggestions(analysis, sceneSuggestionFilter);
      snapshot.sceneSuggestionTotalCount = allVisible.size();
      snapshot.sceneSuggestionFilteredCount = filtered.size();
      if (snapshot.sceneSuggestionsPanelVisible) {
        snapshot.sceneSuggestions.reserve(filtered.size());
      }
      for (const playback_video_analysis::SceneSuggestion* suggestion :
           filtered) {
        if (!snapshot.sceneSuggestionsPanelVisible) break;
        playback_video_edit::SceneSuggestionSnapshot projected =
            playback_video_edit::projectSceneSuggestion(
                *suggestion, document.timeline(),
                selectedSceneSuggestionId == suggestion->id);
        if (projected.spans.empty()) continue;
        snapshot.sceneSuggestions.push_back(std::move(projected));
      }
      const bool selectionVisible =
          selectedSceneSuggestionId &&
          std::any_of(snapshot.sceneSuggestions.begin(),
                      snapshot.sceneSuggestions.end(),
                      [&](const auto& suggestion) {
                        return suggestion.id == *selectedSceneSuggestionId;
                      });
      if (selectionVisible) {
        snapshot.selectedSceneSuggestionId = selectedSceneSuggestionId;
        const auto selected = std::find_if(
            filtered.begin(), filtered.end(), [&](const auto* suggestion) {
              return suggestion->id == *selectedSceneSuggestionId;
            });
        if (selected != filtered.end()) {
          snapshot.selectedSceneSuggestionOrdinal =
              static_cast<size_t>(std::distance(filtered.begin(), selected)) +
              1;
        }
      }
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
    selectedSceneSuggestionId.reset();
    sceneSuggestionsPanelVisible = false;
  }

  const playback_video_analysis::SceneSuggestion* sceneSuggestion(
      const playback_video_analysis::JobSnapshot& analysis,
      uint64_t id) const {
    const auto found = std::find_if(
        analysis.suggestions.begin(), analysis.suggestions.end(),
        [id](const auto& suggestion) { return suggestion.id == id; });
    return found == analysis.suggestions.end() ? nullptr : &*found;
  }

  std::vector<const playback_video_analysis::SceneSuggestion*>
  visibleSceneSuggestions(
      const playback_video_analysis::JobSnapshot& analysis,
      playback_video_edit::SceneSuggestionFilter filter) const {
    std::vector<const playback_video_analysis::SceneSuggestion*> visible;
    visible.reserve(analysis.suggestions.size());
    for (const auto& suggestion : analysis.suggestions) {
      if (dismissedSceneSuggestionIds.count(suggestion.id) == 0 &&
          playback_video_edit::sceneSuggestionMatchesFilter(
              playback_video_edit::projectSceneSuggestionKind(
                  suggestion.kind),
              filter) &&
          playback_video_edit::sceneSuggestionVisibleOnTimeline(
              suggestion, document.timeline())) {
        visible.push_back(&suggestion);
      }
    }
    return visible;
  }

  std::string sceneSuggestionDescription(
      const playback_video_analysis::SceneSuggestion& suggestion) const {
    const playback_video_edit::SceneSuggestionKind kind =
        playback_video_edit::projectSceneSuggestionKind(suggestion.kind);
    std::ostringstream description;
    description << playback_video_edit::sceneSuggestionKindLabel(kind)
                << " ("
                << playback_video_edit::sceneSuggestionStrengthLabel(
                       suggestion.confidence)
                << "): "
                << playback_video_analysis::sceneEvidenceSummary(suggestion);
    return description.str();
  }

  void ensureSelectedSceneSuggestion(
      const playback_video_analysis::JobSnapshot& analysis) {
    const auto suggestions =
        visibleSceneSuggestions(analysis, sceneSuggestionFilter);
    if (suggestions.empty()) {
      selectedSceneSuggestionId.reset();
      return;
    }
    if (selectedSceneSuggestionId &&
        std::any_of(suggestions.begin(), suggestions.end(),
                    [&](const auto* suggestion) {
                      return suggestion->id == *selectedSceneSuggestionId;
                    })) {
      return;
    }
    selectedSceneSuggestionId = suggestions.front()->id;
  }

  VideoEditActionResult startSceneAnalysis(const CommandContext& context) {
    VideoEditActionResult result{true, false, {}};
    playback_video_analysis::JobRequest request;
    request.sourcePath = sourcePath;
    request.videoStreamIndex = context.videoStreamIndex;
    request.durationUs = context.sourceDurationUs;
    request.forceReanalysis =
        sceneAnalysis.snapshot().state ==
        playback_video_analysis::JobState::Succeeded;
    selectedSceneSuggestionId.reset();
    sceneSuggestionsPanelVisible = true;
    dismissedSceneSuggestionIds.clear();
    dismissedSceneSuggestionHistory.clear();
    if (sceneAnalysis.start(std::move(request))) {
      observedSceneAnalysisState =
          playback_video_analysis::JobState::Running;
      observedSceneAnalysisGeneration = sceneAnalysis.snapshot().generation;
      const bool usesTranscript =
          !playback_video_transcript::activeTranscriptPathForVideo(sourcePath)
               .empty();
      result.message = usesTranscript
                           ? "Detecting segments using generated subtitles"
                           : "Detecting segments from video; generate "
                             "subtitles to improve dialogue detection";
    } else if (sceneAnalysis.snapshot().running()) {
      result.message = "Segment detection is already running";
    } else {
      result.message = "Could not start segment detection";
    }
    return result;
  }

  bool focusSceneSuggestion(
      const playback_video_analysis::SceneSuggestion& suggestion,
      const CommandContext& context, bool selectRange) {
    if (dismissedSceneSuggestionIds.count(suggestion.id) != 0) return false;
    selectedSceneSuggestionId = suggestion.id;
    if (selectRange) {
      const int64_t startUs = std::clamp(
          suggestion.startUs, int64_t{0}, context.sourceDurationUs);
      const int64_t endUs = std::clamp(
          suggestion.endUs, startUs, context.sourceDurationUs);
      if (endUs <= startUs) return false;
      selection.clear();
      selection.markIn(document.timeline(), startUs, context.frameDurationUs);
      const int64_t outFrameUs =
          std::max(startUs, endUs - context.frameDurationUs);
      selection.markOut(document.timeline(), outFrameUs, endUs,
                        context.frameDurationUs);
    }
    const std::optional<int64_t> position = positionForSource(
        suggestion.startUs, playback_video_sequence::SourceBias::Forward);
    if (position) (void)player.requestSeek(*position);
    return true;
  }

  VideoEditActionResult navigateSceneSuggestion(
      const CommandContext& context, int direction) {
    VideoEditActionResult result{true, false, {}};
    const playback_video_analysis::JobSnapshot analysis =
        sceneAnalysis.snapshot();
    const auto suggestions =
        visibleSceneSuggestions(analysis, sceneSuggestionFilter);
    if (suggestions.empty()) {
      result.message = "No segment suggestions are available";
      return result;
    }

    size_t target = 0;
    bool selectedRelativeTarget = false;
    if (selectedSceneSuggestionId) {
      const auto current = std::find_if(
          suggestions.begin(), suggestions.end(), [&](const auto* suggestion) {
            return suggestion->id == *selectedSceneSuggestionId;
          });
      if (current != suggestions.end()) {
        const size_t currentIndex = static_cast<size_t>(
            std::distance(suggestions.begin(), current));
        if (direction < 0) {
          target = currentIndex == 0 ? suggestions.size() - 1
                                     : currentIndex - 1;
        } else {
          target = (currentIndex + 1) % suggestions.size();
        }
        selectedRelativeTarget = true;
      }
    }
    if (!selectedRelativeTarget && direction < 0) {
      target = suggestions.size() - 1;
      for (size_t index = suggestions.size(); index > 0; --index) {
        if (suggestions[index - 1]->startUs < context.sourcePositionUs) {
          target = index - 1;
          break;
        }
      }
    } else if (!selectedRelativeTarget) {
      target = 0;
      for (size_t index = 0; index < suggestions.size(); ++index) {
        if (suggestions[index]->startUs > context.sourcePositionUs) {
          target = index;
          break;
        }
      }
    }
    const auto& suggestion = *suggestions[target];
    if (!focusSceneSuggestion(suggestion, context, false)) {
      result.message = "Could not preview the segment suggestion";
      return result;
    }
    result.message = sceneSuggestionDescription(suggestion);
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

  VideoEditActionResult startExport(const CommandContext& context) {
    VideoEditActionResult result;
    result.handled = true;
    const playback_video_edit::ExportSnapshot previous = exporter.snapshot();
    if (previous.running()) {
      result.message = "An export is already running";
      return result;
    }
    // Starting a new worker replaces its terminal snapshot. Reconcile a
    // successful predecessor first so its immutable output revision can never
    // be lost in the UI-thread/worker completion race.
    const bool completedCurrentRevision =
        document.hasUnexportedChanges() &&
        previous.state == playback_video_edit::ExportState::Succeeded &&
        previous.decisions == document.timeline().decisionList();
    if (previous.state == playback_video_edit::ExportState::Succeeded) {
      document.markExported(previous.decisions);
    }
    if (completedCurrentRevision) {
      exporter.consumeChanged();
      observedExportState = playback_video_edit::ExportState::Succeeded;
      result.message =
          "Exported " + toUtf8String(previous.destinationPath.filename());
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
    observedExportState = playback_video_edit::ExportState::Running;
    result.exportStarted = true;
    result.message =
        "Export started: " +
        toUtf8String(destination.filename());
    return result;
  }

  VideoEditActionResult cancelExport() {
    if (exporter.snapshot().running()) {
      exporter.cancel();
      return {true, false, "Cancelling edit export..."};
    }
    return {true, false, "No export is running"};
  }

  VideoEditActionResult finish() {
    const bool changesRetained = document.hasUnexportedChanges();
    finishEditing();
    return {true, false,
            changesRetained ? "Edit mode closed; changes retained"
                            : "Edit mode closed"};
  }
};

VideoEditWorkspace::VideoEditWorkspace(
    std::filesystem::path sourcePath, Player& player,
    playback_video_timeline_preview::HoverModel& timelinePreview,
    playback_video_timeline_preview::Provider& timelinePreviewProvider)
    : impl_(std::make_unique<Impl>(std::move(sourcePath), player,
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
    case playback_video_edit::Command::CancelSceneAnalysis:
      if (impl_->sceneAnalysis.snapshot().running()) {
        impl_->sceneAnalysis.cancel();
        result.message = "Cancelling segment detection...";
      } else {
        result.message = "Segment detection is not running";
      }
      break;
    case playback_video_edit::Command::ToggleSceneSuggestions: {
      const playback_video_analysis::JobSnapshot analysis =
          impl_->sceneAnalysis.snapshot();
      if (analysis.running()) {
        impl_->sceneAnalysis.cancel();
        result.message = "Cancelling segment detection...";
      } else if (analysis.state ==
                 playback_video_analysis::JobState::Succeeded) {
        impl_->sceneSuggestionsPanelVisible =
            !impl_->sceneSuggestionsPanelVisible;
        if (impl_->sceneSuggestionsPanelVisible) {
          impl_->ensureSelectedSceneSuggestion(analysis);
        }
        result.message = impl_->sceneSuggestionsPanelVisible
                             ? "Suggestions shown"
                             : "Suggestions hidden";
      } else {
        result = impl_->startSceneAnalysis(context);
      }
      break;
    }
    case playback_video_edit::Command::CycleSceneSuggestionFilter: {
      impl_->sceneSuggestionFilter =
          playback_video_edit::nextSceneSuggestionFilter(
              impl_->sceneSuggestionFilter);
      const playback_video_analysis::JobSnapshot analysis =
          impl_->sceneAnalysis.snapshot();
      impl_->ensureSelectedSceneSuggestion(analysis);
      const auto filtered = impl_->visibleSceneSuggestions(
          analysis, impl_->sceneSuggestionFilter);
      result.message =
          std::string("Suggestion filter: ") +
          playback_video_edit::sceneSuggestionFilterLabel(
              impl_->sceneSuggestionFilter) +
          " (" + std::to_string(filtered.size()) + ")";
      break;
    }
    case playback_video_edit::Command::PreviousSceneSuggestion:
      result = impl_->navigateSceneSuggestion(context, -1);
      break;
    case playback_video_edit::Command::NextSceneSuggestion:
      result = impl_->navigateSceneSuggestion(context, 1);
      break;
    case playback_video_edit::Command::SelectSceneSuggestion: {
      const playback_video_analysis::JobSnapshot analysis =
          impl_->sceneAnalysis.snapshot();
      const auto* suggestion =
          impl_->selectedSceneSuggestionId
              ? impl_->sceneSuggestion(analysis,
                                       *impl_->selectedSceneSuggestionId)
              : nullptr;
      if (!suggestion ||
          impl_->dismissedSceneSuggestionIds.count(suggestion->id) != 0) {
        result.message = "Choose a segment suggestion first";
      } else if (!impl_->focusSceneSuggestion(*suggestion, context, true)) {
        result.message = "Could not select that suggested scene";
      } else {
        result.message = "Selected " +
                         impl_->sceneSuggestionDescription(*suggestion);
      }
      break;
    }
    case playback_video_edit::Command::DismissSceneSuggestion:
      if (impl_->selectedSceneSuggestionId) {
        const playback_video_analysis::JobSnapshot analysis =
            impl_->sceneAnalysis.snapshot();
        const auto before = impl_->visibleSceneSuggestions(
            analysis, impl_->sceneSuggestionFilter);
        const auto selected = std::find_if(
            before.begin(), before.end(), [&](const auto* suggestion) {
              return suggestion->id == *impl_->selectedSceneSuggestionId;
            });
        if (selected == before.end()) {
          impl_->selectedSceneSuggestionId.reset();
          result.message = "Choose a segment suggestion first";
          break;
        }
        const size_t selectedIndex =
            static_cast<size_t>(std::distance(before.begin(), selected));
        const uint64_t dismissedId = (*selected)->id;
        if (impl_->dismissedSceneSuggestionIds.insert(dismissedId).second) {
          impl_->dismissedSceneSuggestionHistory.push_back(dismissedId);
        }
        const auto after = impl_->visibleSceneSuggestions(
            analysis, impl_->sceneSuggestionFilter);
        impl_->selectedSceneSuggestionId =
            after.empty()
                ? std::optional<uint64_t>{}
                : std::optional<uint64_t>{
                      after[std::min(selectedIndex, after.size() - 1)]->id};
        result.message = "Suggestion hidden (Undo hide is available)";
      } else {
        result.message = "Choose a segment suggestion first";
      }
      break;
    case playback_video_edit::Command::UndoDismissSceneSuggestion: {
      bool restored = false;
      while (!impl_->dismissedSceneSuggestionHistory.empty()) {
        const uint64_t id = impl_->dismissedSceneSuggestionHistory.back();
        impl_->dismissedSceneSuggestionHistory.pop_back();
        if (impl_->dismissedSceneSuggestionIds.erase(id) == 0) continue;
        const playback_video_analysis::JobSnapshot analysis =
            impl_->sceneAnalysis.snapshot();
        if (const auto* suggestion = impl_->sceneSuggestion(analysis, id)) {
          const auto kind =
              playback_video_edit::projectSceneSuggestionKind(
                  suggestion->kind);
          if (!playback_video_edit::sceneSuggestionMatchesFilter(
                  kind, impl_->sceneSuggestionFilter)) {
            impl_->sceneSuggestionFilter =
                playback_video_edit::SceneSuggestionFilter::All;
          }
        }
        impl_->selectedSceneSuggestionId = id;
        impl_->sceneSuggestionsPanelVisible = true;
        restored = true;
        break;
      }
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
    if (impl_->selectedSceneSuggestionId) {
      const playback_video_analysis::JobSnapshot analysis =
          impl_->sceneAnalysis.snapshot();
      const auto* selected = impl_->sceneSuggestion(
          analysis, *impl_->selectedSceneSuggestionId);
      if (!selected ||
          !playback_video_edit::sceneSuggestionVisibleOnTimeline(
              *selected, impl_->document.timeline())) {
        impl_->selectedSceneSuggestionId.reset();
      }
    }
  }

  if (!projectionAccepted) {
    result.message = "Timeline changed; preview unavailable";
  }

  result.pausePlayback =
      result.handled && command == playback_video_edit::Command::Open &&
      impl_->active;
  return result;
}

VideoEditActionResult VideoEditWorkspace::navigateBack() {
  VideoEditActionResult result;
  if (!impl_) return result;
  if (impl_->prompt != playback_video_edit::Prompt::None) {
    return execute(playback_video_edit::Command::CancelPrompt);
  }
  if (!impl_->active) return result;
  if (impl_->selection.hasMarks()) {
    return execute(playback_video_edit::Command::ClearInAndOut);
  }
  if (impl_->sceneSuggestionsPanelVisible) {
    impl_->sceneSuggestionsPanelVisible = false;
    impl_->selectedSceneSuggestionId.reset();
    return {true, false, "Suggestions hidden"};
  }
  if (impl_->selectedSceneSuggestionId) {
    impl_->selectedSceneSuggestionId.reset();
    return {true, false, "Segment suggestion focus cleared"};
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
  const auto appendMessage = [&](const std::string& message) {
    if (message.empty()) return;
    if (!result.message.empty()) result.message += " | ";
    result.message += message;
  };

  if (impl_->exporter.consumeChanged()) {
    result.changed = true;
    const playback_video_edit::ExportState previous =
        impl_->observedExportState;
    const playback_video_edit::ExportSnapshot state =
        impl_->exporter.snapshot();
    impl_->observedExportState = state.state;
    if (previous == playback_video_edit::ExportState::Running &&
        state.finished()) {
      switch (state.state) {
        case playback_video_edit::ExportState::Succeeded:
          impl_->document.markExported(state.decisions);
          result.completion = VideoEditExportCompletion::Succeeded;
          appendMessage(
              "Exported " + toUtf8String(state.destinationPath.filename()));
          break;
        case playback_video_edit::ExportState::Failed:
          result.completion = VideoEditExportCompletion::Failed;
          appendMessage("Export failed: " + state.error);
          break;
        case playback_video_edit::ExportState::Cancelled:
          result.completion = VideoEditExportCompletion::Cancelled;
          appendMessage("Export cancelled");
          break;
        default:
          break;
      }
    }
  }

  if (impl_->sceneAnalysis.consumeChanged()) {
    result.changed = true;
    const playback_video_analysis::JobState previous =
        impl_->observedSceneAnalysisState;
    const playback_video_analysis::JobSnapshot state =
        impl_->sceneAnalysis.snapshot();
    const bool sameGeneration =
        state.generation == impl_->observedSceneAnalysisGeneration;
    impl_->observedSceneAnalysisState = state.state;
    impl_->observedSceneAnalysisGeneration = state.generation;
    if (sameGeneration &&
        previous == playback_video_analysis::JobState::Running &&
        state.finished()) {
      switch (state.state) {
        case playback_video_analysis::JobState::Succeeded: {
          impl_->sceneSuggestionsPanelVisible = true;
          const auto visible = impl_->visibleSceneSuggestions(
              state, impl_->sceneSuggestionFilter);
          if (!visible.empty()) {
            const CommandContext context = impl_->commandContext();
            const auto current = std::find_if(
                visible.begin(), visible.end(), [&](const auto* suggestion) {
                  return suggestion->startUs <= context.sourcePositionUs &&
                         suggestion->endUs > context.sourcePositionUs;
                });
            impl_->selectedSceneSuggestionId =
                (current == visible.end() ? visible.front() : *current)->id;
          }
          std::string message =
              "Segment detection complete: " +
              std::to_string(state.suggestions.size()) + " suggestions";
          if (state.usedIndexedTranscript) {
            message += " using generated subtitles";
          }
          appendMessage(message);
          break;
        }
        case playback_video_analysis::JobState::Failed:
          appendMessage("Segment detection failed: " + state.error);
          break;
        case playback_video_analysis::JobState::Cancelled:
          appendMessage("Segment detection cancelled");
          break;
        default:
          break;
      }
    }
  }
  return result;
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
          snapshot.sceneSuggestions, timelineUs,
          std::max<int64_t>(0, toleranceUs));
  impl_->selectedSceneSuggestionId = suggestion;
  return suggestion.has_value();
}

void VideoEditWorkspace::clearCutSelection() {
  if (impl_) impl_->selectedCut.reset();
}

void VideoEditWorkspace::stop() {
  if (!impl_) return;
  impl_->sceneAnalysis.stop();
  impl_->observedSceneAnalysisState =
      impl_->sceneAnalysis.snapshot().state;
  impl_->exporter.stop();
  impl_->observedExportState = impl_->exporter.snapshot().state;
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
