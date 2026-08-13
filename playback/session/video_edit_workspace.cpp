#include "playback/session/video_edit_workspace.h"

#include <algorithm>
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

enum class PreviewProjection : uint8_t {
  None,
  ApplyEdit,
  ClearEdit,
};

struct CommandContext {
  int64_t sourceDurationUs = 0;
  int64_t playheadUs = 0;
  int64_t frameDurationUs = 1;
  int videoStreamIndex = -1;
  int audioStreamIndex = -1;
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
  bool active = false;
  playback_video_edit::Prompt prompt = playback_video_edit::Prompt::None;
  playback_video_edit::Exporter exporter;
  playback_video_edit::ExportState observedExportState =
      playback_video_edit::ExportState::Idle;

  CommandContext commandContext() const {
    CommandContext context;
    context.sourceDurationUs = player.sourceDurationUs();
    const PlayerTimelineSnapshot timeline = player.timelineSnapshot();
    context.playheadUs = std::clamp(
        timeline.sourcePositionUs, int64_t{0},
        std::max<int64_t>(0, context.sourceDurationUs));
    context.frameDurationUs =
        std::max<int64_t>(1, timeline.frameDurationUs);
    context.videoStreamIndex = player.videoStreamIndex();
    context.audioStreamIndex = player.activeAudioStreamIndex();
    return context;
  }

  playback_video_edit::EditSnapshot editSnapshot() const {
    std::optional<int64_t> playheadTimelineUs;
    int64_t frameDurationUs = 0;
    if (active) {
      const PlayerTimelineSnapshot timeline = player.timelineSnapshot();
      playheadTimelineUs = timeline.positionUs;
      frameDurationUs = timeline.frameDurationUs;
    }
    return playback_video_edit::buildSnapshot(
        document, selection, active, playheadTimelineUs, frameDurationUs);
  }

  playback_video_edit::ExportProgress exportProgressSnapshot() const {
    const playback_video_edit::ExportSnapshot state = exporter.snapshot();
    return {state.running(), state.progress};
  }

  void updateTimelinePreview(
      const std::vector<playback_video_edit::SourceRange>& ranges) {
    if (!timelinePreview.setSequence(ranges)) return;
    timelinePreviewProvider.cancelBefore(timelinePreview.requestId());
  }

  bool applyEditPreview() {
    const auto& ranges = document.timeline().keptRanges();
    if (!player.setPlaybackSequence(ranges)) return false;
    updateTimelinePreview(ranges);
    return true;
  }

  bool clearEditPreview() {
    if (!player.clearPlaybackSequence()) return false;
    const int64_t durationUs = player.sourceDurationUs();
    if (durationUs > 0) updateTimelinePreview({{0, durationUs}});
    return true;
  }

  VideoEditActionResult startOrCancelExport(const CommandContext& context) {
    VideoEditActionResult result;
    result.handled = true;
    const playback_video_edit::ExportSnapshot state = exporter.snapshot();
    if (state.running()) {
      exporter.cancel();
      result.message = "Cancelling edit export...";
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
    request.keptRanges = timeline.keptRanges();
    request.videoStreamIndex = context.videoStreamIndex;
    request.audioStreamIndex = context.audioStreamIndex;
    if (!exporter.start(std::move(request))) {
      result.message = "Could not start edit export";
      return result;
    }
    observedExportState = playback_video_edit::ExportState::Running;
    result.message =
        "Export started: " +
        toUtf8String(destination.filename());
    return result;
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
  return impl_ &&
         (impl_->document.hasUnexportedChanges() ||
          impl_->exporter.snapshot().running());
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
      command != playback_video_edit::Command::Export &&
      command != playback_video_edit::Command::RequestDiscard &&
      !promptCommand) {
    return result;
  }
  result.handled = true;

  const CommandContext context = impl_->commandContext();
  bool timelineChanged = false;
  PreviewProjection projection = PreviewProjection::None;
  bool deactivateAfterClear = false;
  bool deactivateIfOpenFails = false;

  switch (command) {
    case playback_video_edit::Command::Open:
      if (impl_->active) {
        result.handled = false;
      } else if (context.sourceDurationUs > 0) {
        impl_->document.load(context.sourceDurationUs);
        impl_->selection.clear();
        impl_->active = true;
        impl_->prompt = playback_video_edit::Prompt::None;
        projection = PreviewProjection::ApplyEdit;
        deactivateIfOpenFails = true;
        result.message = "Video editor opened";
      } else {
        result.message = "Video editor requires a known duration";
      }
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
        impl_->prompt = playback_video_edit::Prompt::None;
        projection = PreviewProjection::ClearEdit;
        deactivateAfterClear = true;
        result.message = "Edit mode closed; edits retained";
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
      impl_->selection.markIn(impl_->document.timeline(), context.playheadUs);
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
            std::clamp(context.playheadUs, int64_t{0}, duration);
        const int64_t frameDuration =
            std::max<int64_t>(1, context.frameDurationUs);
        const int64_t out = frameDuration >= duration - playhead
                                ? duration
                                : playhead + frameDuration;
        impl_->selection.markOut(impl_->document.timeline(), out);
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
      if (const auto selected = impl_->selection.range()) {
        timelineChanged = impl_->document.rippleDelete(*selected);
      }
      result.message = timelineChanged ? "Range deleted (ripple)"
                                       : "Set a non-empty In/Out range first";
      break;
    case playback_video_edit::Command::Trim:
      if (const auto selected = impl_->selection.range()) {
        timelineChanged = impl_->document.trimTo(*selected);
      }
      result.message = timelineChanged ? "Sequence trimmed to selection"
                                       : "Set a non-empty In/Out range first";
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
    case playback_video_edit::Command::Export:
      result = impl_->startOrCancelExport(context);
      break;
  }

  if (timelineChanged) {
    impl_->selection.clear();
    if (impl_->active) projection = PreviewProjection::ApplyEdit;
  }

  bool projectionAccepted = true;
  if (projection == PreviewProjection::ApplyEdit) {
    projectionAccepted = impl_->applyEditPreview();
  } else if (projection == PreviewProjection::ClearEdit) {
    projectionAccepted = impl_->clearEditPreview();
  }

  if (!projectionAccepted) {
    if (deactivateIfOpenFails) {
      impl_->active = false;
      impl_->prompt = playback_video_edit::Prompt::None;
      impl_->selection.clear();
      result.message = "Video editor unavailable: preview could not start";
    } else if (deactivateAfterClear) {
      result.message = "Editor remains open: source preview unavailable";
    } else {
      result.message = "Timeline changed; preview unavailable";
    }
  } else if (deactivateAfterClear) {
    impl_->active = false;
    impl_->prompt = playback_video_edit::Prompt::None;
    impl_->selection.clear();
  }

  result.pausePlayback =
      command == playback_video_edit::Command::Open &&
      projectionAccepted && impl_->active;
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

bool VideoEditWorkspace::poll(std::string* message) {
  if (!impl_ || !impl_->exporter.consumeChanged()) return false;
  const playback_video_edit::ExportState previous =
      impl_->observedExportState;
  const playback_video_edit::ExportSnapshot state = impl_->exporter.snapshot();
  impl_->observedExportState = state.state;
  if (message) message->clear();
  if (previous != playback_video_edit::ExportState::Running ||
      !state.finished()) {
    return true;
  }
  switch (state.state) {
    case playback_video_edit::ExportState::Succeeded:
      impl_->document.markExported(state.keptRanges);
      if (message) {
        *message =
            "Exported " +
            toUtf8String(state.destinationPath.filename());
      }
      break;
    case playback_video_edit::ExportState::Failed:
      if (message) *message = "Export failed: " + state.error;
      break;
    case playback_video_edit::ExportState::Cancelled:
      if (message) *message = "Export cancelled";
      break;
    default:
      break;
  }
  return true;
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
