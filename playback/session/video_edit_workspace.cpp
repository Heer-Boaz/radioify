#include "playback/session/video_edit_workspace.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "core/runtime_helpers.h"
#include "playback/video/edit/export.h"
#include "playback/video/edit/timeline.h"
#include "playback/video/player.h"
#include "playback/video/timeline_preview.h"
#include "playback/video/timeline_preview_model.h"

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
                              context.sourcePositionUs);
      result.message = "In point set";
      break;
    case playback_video_edit::Command::ClearIn:
      result.message =
          impl_->selection.clear(playback_video_edit::EditBoundary::In)
              ? "In point cleared"
              : "No In point to clear";
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
        impl_->selection.markOut(impl_->document.timeline(), playhead, out);
      }
      result.message = "Out point set";
      break;
    case playback_video_edit::Command::ClearOut:
      result.message =
          impl_->selection.clear(playback_video_edit::EditBoundary::Out)
              ? "Out point cleared"
              : "No Out point to clear";
      break;
    case playback_video_edit::Command::ClearInAndOut:
      result.message = impl_->selection.clear()
                           ? "In/Out points cleared"
                           : "No In/Out points to clear";
      break;
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
        result.message = timelineChanged ? "Range deleted (ripple)"
                                         : "That range cannot be deleted";
      } else {
        result.message = "Set both In and Out points first";
      }
      break;
    case playback_video_edit::Command::Trim:
      if (const auto keep =
              impl_->selection.trimRange(impl_->document.timeline())) {
        timelineChanged = impl_->document.trimTo(*keep);
        // A trimmed program is reviewed from its new sequence origin.
        if (timelineChanged) nextPositionUs = 0;
        result.message = timelineChanged ? "Sequence trimmed"
                                         : "Trim point does not change sequence";
      } else {
        result.message = "Set an In or Out point first";
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
  if (!impl_ || !impl_->exporter.consumeChanged()) return result;
  result.changed = true;
  const playback_video_edit::ExportState previous =
      impl_->observedExportState;
  const playback_video_edit::ExportSnapshot state = impl_->exporter.snapshot();
  impl_->observedExportState = state.state;
  if (previous != playback_video_edit::ExportState::Running ||
      !state.finished()) {
    return result;
  }
  switch (state.state) {
    case playback_video_edit::ExportState::Succeeded:
      impl_->document.markExported(state.decisions);
      result.completion = VideoEditExportCompletion::Succeeded;
      result.message =
          "Exported " + toUtf8String(state.destinationPath.filename());
      break;
    case playback_video_edit::ExportState::Failed:
      result.completion = VideoEditExportCompletion::Failed;
      result.message = "Export failed: " + state.error;
      break;
    case playback_video_edit::ExportState::Cancelled:
      result.completion = VideoEditExportCompletion::Cancelled;
      result.message = "Export cancelled";
      break;
    default:
      break;
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

void VideoEditWorkspace::clearCutSelection() {
  if (impl_) impl_->selectedCut.reset();
}

void VideoEditWorkspace::stop() {
  if (!impl_) return;
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
