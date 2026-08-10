#include "playback/video/edit/controller.h"

#include <algorithm>
#include <utility>

#include "core/runtime_helpers.h"
#include "playback/video/edit/export.h"

namespace playback_video_edit {
struct Controller::Impl {
  explicit Impl(std::filesystem::path path) : sourcePath(std::move(path)) {
    refreshView();
  }

  std::filesystem::path sourcePath;
  EditSession session;
  Exporter exporter;
  ExportSnapshot exportState;
  EditSnapshot editView;
  ExportProgress exportView;
  std::vector<SourceRange> pendingExportRanges;
  std::vector<SourceRange> lastExportedRanges;

  void refreshView() {
    editView = session.snapshot();
    const Timeline& timeline = session.timeline();
    editView.dirty =
        !timeline.isUnmodified() &&
        (lastExportedRanges.empty() ||
         timeline.keptRanges() != lastExportedRanges);
    exportView.active = exportState.running();
    exportView.fraction = exportState.progress;
  }

  CommandResult startOrCancelExport(const CommandContext& context) {
    CommandResult result;
    result.handled = true;
    exportState = exporter.snapshot();
    if (exportState.running()) {
      exporter.cancel();
      result.message = "Cancelling edit export...";
      return result;
    }
    const Timeline& timeline = session.timeline();
    if (timeline.sourceDurationUs() <= 0) {
      result.message = "Open the video editor and make an edit first";
      return result;
    }
    if (timeline.isUnmodified()) {
      result.message = "Make at least one trim or cut before exporting";
      return result;
    }
    const std::filesystem::path destination =
        uniqueEditedOutputPath(sourcePath);
    if (destination.empty()) {
      result.message = "Could not select a safe output filename";
      return result;
    }

    ExportRequest request;
    request.sourcePath = sourcePath;
    request.destinationPath = destination;
    request.keptRanges = timeline.keptRanges();
    request.videoStreamIndex = context.videoStreamIndex;
    request.audioStreamIndex = context.audioStreamIndex;
    pendingExportRanges = request.keptRanges;
    if (!exporter.start(std::move(request))) {
      pendingExportRanges.clear();
      result.message = "Could not start edit export";
      return result;
    }
    exportState = exporter.snapshot();
    result.message =
        "Export started: " + toUtf8String(destination.filename());
    return result;
  }
};

Controller::Controller(std::filesystem::path sourcePath)
    : impl_(std::make_unique<Impl>(std::move(sourcePath))) {}

Controller::~Controller() = default;

bool Controller::active() const { return impl_ && impl_->session.active(); }

bool Controller::hasUnexportedChanges() const {
  return impl_ && impl_->editView.dirty;
}

bool Controller::moveBoundary(EditBoundary boundary, int64_t timelineUs,
                              int64_t minimumSelectionDurationUs) {
  if (!impl_) return false;
  const bool changed = impl_->session.moveBoundary(
      boundary, timelineUs, minimumSelectionDurationUs);
  if (changed) impl_->refreshView();
  return changed;
}

CommandResult Controller::execute(Command command,
                                  const CommandContext& context) {
  CommandResult result;
  if (!impl_) return result;
  result.handled = true;

  bool timelineChanged = false;
  switch (command) {
    case Command::Toggle:
      if (impl_->session.active()) {
        result.sequenceEffect = CommandResult::SequenceEffect::Clear;
        result.deactivateAfterSequenceClear = true;
        result.message = "Video editor closed (edits retained)";
      } else if (context.sourceDurationUs > 0) {
        impl_->session.activate(context.sourceDurationUs);
        result.sequenceEffect = CommandResult::SequenceEffect::Apply;
        result.message = "Video editor opened";
      } else {
        result.message = "Video editor requires a known duration";
      }
      break;
    case Command::Close:
      if (!impl_->session.active()) {
        result.handled = false;
        break;
      }
      result.sequenceEffect = CommandResult::SequenceEffect::Clear;
      result.deactivateAfterSequenceClear = true;
      result.message = "Video editor closed (edits retained)";
      break;
    case Command::MarkIn:
      if (!impl_->session.active()) {
        result.handled = false;
        break;
      }
      impl_->session.markIn(context.playheadUs);
      result.message = "In point set";
      break;
    case Command::MarkOut:
      if (!impl_->session.active()) {
        result.handled = false;
        break;
      }
      {
        const int64_t duration = std::max<int64_t>(0, context.sourceDurationUs);
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
    case Command::RippleDelete:
      timelineChanged = impl_->session.rippleDeleteSelection();
      result.message = timelineChanged ? "Selection removed (ripple)"
                                       : "Set a non-empty In/Out range first";
      break;
    case Command::Trim:
      timelineChanged = impl_->session.trimToSelection();
      result.message = timelineChanged ? "Sequence trimmed to selection"
                                       : "Set a non-empty In/Out range first";
      break;
    case Command::Undo:
      timelineChanged = impl_->session.undo();
      result.message = timelineChanged ? "Edit undone" : "Nothing to undo";
      break;
    case Command::Redo:
      timelineChanged = impl_->session.redo();
      result.message = timelineChanged ? "Edit redone" : "Nothing to redo";
      break;
    case Command::Reset:
      timelineChanged = impl_->session.resetEdits();
      result.message = timelineChanged ? "All cuts reset"
                                       : "Sequence is unchanged";
      break;
    case Command::Export:
      result = impl_->startOrCancelExport(context);
      break;
  }

  if (timelineChanged) {
    result.sequenceEffect = CommandResult::SequenceEffect::Apply;
  }
  impl_->refreshView();
  return result;
}

void Controller::completeSequenceEffect(const CommandResult& result,
                                        bool accepted) {
  if (!impl_ || !accepted || !result.deactivateAfterSequenceClear) return;
  impl_->session.deactivate();
  impl_->refreshView();
}

bool Controller::poll(std::string* message) {
  if (!impl_ || !impl_->exporter.consumeChanged()) return false;
  const ExportState previous = impl_->exportState.state;
  impl_->exportState = impl_->exporter.snapshot();
  impl_->refreshView();
  if (message) message->clear();
  if (previous != ExportState::Running || !impl_->exportState.finished()) {
    return true;
  }
  switch (impl_->exportState.state) {
    case ExportState::Succeeded:
      impl_->lastExportedRanges = impl_->pendingExportRanges;
      impl_->pendingExportRanges.clear();
      if (message) {
        *message = "Exported " +
                   toUtf8String(impl_->exportState.destinationPath.filename());
      }
      break;
    case ExportState::Failed:
      impl_->pendingExportRanges.clear();
      if (message) *message = "Export failed: " + impl_->exportState.error;
      break;
    case ExportState::Cancelled:
      impl_->pendingExportRanges.clear();
      if (message) *message = "Export cancelled";
      break;
    default:
      break;
  }
  impl_->refreshView();
  return true;
}

void Controller::stop() {
  if (!impl_) return;
  impl_->exporter.stop();
  impl_->exportState = impl_->exporter.snapshot();
  impl_->refreshView();
}

const EditSnapshot& Controller::edit() const { return impl_->editView; }

const ExportProgress& Controller::exportProgress() const {
  return impl_->exportView;
}

}  // namespace playback_video_edit
