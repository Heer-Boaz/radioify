#include "playback/session/video_edit_workspace.h"

#include <algorithm>
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
        timelinePreviewProvider(previewProvider) {
    refreshView();
  }

  std::filesystem::path sourcePath;
  Player& player;
  playback_video_timeline_preview::HoverModel& timelinePreview;
  playback_video_timeline_preview::Provider& timelinePreviewProvider;
  playback_video_edit::EditSession session;
  playback_video_edit::Exporter exporter;
  playback_video_edit::ExportSnapshot exportState;
  playback_video_edit::EditSnapshot editView;
  playback_video_edit::ExportProgress exportView;

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

  void refreshPlayhead() {
    editView.playheadTimelineUs.reset();
    editView.frameDurationUs = 0;
    if (!editView.active) return;
    const PlayerTimelineSnapshot timeline = player.timelineSnapshot();
    editView.playheadTimelineUs = timeline.positionUs;
    editView.frameDurationUs = std::max<int64_t>(0, timeline.frameDurationUs);
  }

  void refreshView() {
    editView = session.snapshot();
    exportView.active = exportState.running();
    exportView.fraction = exportState.progress;
    refreshPlayhead();
  }

  void updateTimelinePreview(
      const std::vector<playback_video_edit::SourceRange>& ranges) {
    if (!timelinePreview.setSequence(ranges)) return;
    timelinePreviewProvider.cancelBefore(timelinePreview.requestId());
  }

  bool applyEditPreview() {
    const auto& ranges = session.timeline().keptRanges();
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
    exportState = exporter.snapshot();
    if (exportState.running()) {
      exporter.cancel();
      result.message = "Cancelling edit export...";
      return result;
    }
    const playback_video_edit::Timeline& timeline = session.timeline();
    if (timeline.sourceDurationUs() <= 0) {
      result.message = "Open the video editor and make an edit first";
      return result;
    }
    if (timeline.isUnmodified()) {
      result.message = "Make at least one trim or cut before exporting";
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
    exportState = exporter.snapshot();
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
  return impl_ && impl_->session.active();
}

bool VideoEditWorkspace::hasUnexportedChanges() const {
  return impl_ && impl_->session.hasUnexportedChanges();
}

bool VideoEditWorkspace::needsExitConfirmation() const {
  return impl_ &&
         (impl_->session.hasUnexportedChanges() || impl_->exportState.running());
}

VideoEditActionResult VideoEditWorkspace::execute(
    playback_video_edit::Command command) {
  VideoEditActionResult result;
  if (!impl_) return result;
  result.handled = true;

  const CommandContext context = impl_->commandContext();
  bool timelineChanged = false;
  PreviewProjection projection = PreviewProjection::None;
  bool deactivateAfterClear = false;
  bool deactivateIfOpenFails = false;

  switch (command) {
    case playback_video_edit::Command::Open:
      if (impl_->session.active()) {
        result.handled = false;
      } else if (context.sourceDurationUs > 0) {
        impl_->session.activate(context.sourceDurationUs);
        projection = PreviewProjection::ApplyEdit;
        deactivateIfOpenFails = true;
        result.message = "Video editor opened";
      } else {
        result.message = "Video editor requires a known duration";
      }
      break;
    case playback_video_edit::Command::Close:
      if (!impl_->session.active()) {
        result.handled = false;
        break;
      }
      projection = PreviewProjection::ClearEdit;
      deactivateAfterClear = true;
      result.message = "Video editor closed (edits retained)";
      break;
    case playback_video_edit::Command::MarkIn:
      if (!impl_->session.active()) {
        result.handled = false;
        break;
      }
      impl_->session.markIn(context.playheadUs);
      result.message = "In point set";
      break;
    case playback_video_edit::Command::MarkOut:
      if (!impl_->session.active()) {
        result.handled = false;
        break;
      }
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
        impl_->session.markOut(out);
      }
      result.message = "Out point set";
      break;
    case playback_video_edit::Command::RippleDelete:
      timelineChanged = impl_->session.rippleDeleteSelection();
      result.message = timelineChanged ? "Selection removed (ripple)"
                                       : "Set a non-empty In/Out range first";
      break;
    case playback_video_edit::Command::Trim:
      timelineChanged = impl_->session.trimToSelection();
      result.message = timelineChanged ? "Sequence trimmed to selection"
                                       : "Set a non-empty In/Out range first";
      break;
    case playback_video_edit::Command::Undo:
      timelineChanged = impl_->session.undo();
      result.message = timelineChanged ? "Edit undone" : "Nothing to undo";
      break;
    case playback_video_edit::Command::Redo:
      timelineChanged = impl_->session.redo();
      result.message = timelineChanged ? "Edit redone" : "Nothing to redo";
      break;
    case playback_video_edit::Command::Reset:
      timelineChanged = impl_->session.resetEdits();
      result.message = timelineChanged ? "All cuts reset"
                                       : "Sequence is unchanged";
      break;
    case playback_video_edit::Command::Export:
      result = impl_->startOrCancelExport(context);
      break;
    case playback_video_edit::Command::Discard:
      impl_->exportState = impl_->exporter.snapshot();
      if (impl_->exportState.running()) {
        result.message = "Cancel the active save before discarding changes";
        break;
      }
      timelineChanged = impl_->session.discardAllChanges();
      result.message = timelineChanged ? "Edits discarded"
                                       : "There are no edits to discard";
      break;
  }

  if (timelineChanged && impl_->session.active()) {
    projection = PreviewProjection::ApplyEdit;
  }

  bool projectionAccepted = true;
  if (projection == PreviewProjection::ApplyEdit) {
    projectionAccepted = impl_->applyEditPreview();
  } else if (projection == PreviewProjection::ClearEdit) {
    projectionAccepted = impl_->clearEditPreview();
  }

  if (!projectionAccepted) {
    if (deactivateIfOpenFails) {
      impl_->session.deactivate();
      result.message = "Video editor unavailable: preview could not start";
    } else if (deactivateAfterClear) {
      result.message = "Editor remains open: source preview unavailable";
    } else {
      result.message = "Timeline changed; preview unavailable";
    }
  } else if (deactivateAfterClear) {
    impl_->session.deactivate();
  }

  result.pausePlayback =
      command == playback_video_edit::Command::Open &&
      projectionAccepted && impl_->session.active();
  impl_->refreshView();
  return result;
}

bool VideoEditWorkspace::moveBoundary(
    playback_video_edit::EditBoundary boundary, int64_t timelineUs) {
  if (!impl_ || !impl_->session.active()) return false;
  const int64_t minimumDurationUs = std::max<int64_t>(
      1, impl_->player.timelineSnapshot().frameDurationUs);
  if (!impl_->session.moveBoundary(boundary, timelineUs,
                                   minimumDurationUs)) {
    return false;
  }
  impl_->refreshView();
  return true;
}

bool VideoEditWorkspace::poll(std::string* message) {
  if (!impl_ || !impl_->exporter.consumeChanged()) return false;
  const playback_video_edit::ExportState previous = impl_->exportState.state;
  impl_->exportState = impl_->exporter.snapshot();
  if (message) message->clear();
  if (previous != playback_video_edit::ExportState::Running ||
      !impl_->exportState.finished()) {
    impl_->refreshView();
    return true;
  }
  switch (impl_->exportState.state) {
    case playback_video_edit::ExportState::Succeeded:
      impl_->session.markExported(impl_->exportState.keptRanges);
      if (message) {
        *message =
            "Exported " +
            toUtf8String(impl_->exportState.destinationPath.filename());
      }
      break;
    case playback_video_edit::ExportState::Failed:
      if (message) *message = "Export failed: " + impl_->exportState.error;
      break;
    case playback_video_edit::ExportState::Cancelled:
      if (message) *message = "Export cancelled";
      break;
    default:
      break;
  }
  impl_->refreshView();
  return true;
}

void VideoEditWorkspace::refreshPlayhead() {
  if (impl_) impl_->refreshPlayhead();
}

void VideoEditWorkspace::stop() {
  if (!impl_) return;
  impl_->exporter.stop();
  impl_->exportState = impl_->exporter.snapshot();
  impl_->refreshView();
}

const playback_video_edit::EditSnapshot& VideoEditWorkspace::edit() const {
  return impl_->editView;
}

const playback_video_edit::ExportProgress&
VideoEditWorkspace::exportProgress() const {
  return impl_->exportView;
}

}  // namespace playback_session
