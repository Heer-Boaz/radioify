#include "playback/video/edit/timeline.h"
#include "playback/video/edit/preview.h"
#include "playback/video/edit/overlay_model.h"

#include <iostream>
#include <optional>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "video_edit_timeline_tests: " << message << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  using playback_video_edit::EditSession;
  using playback_video_edit::PreviewController;
  using playback_video_edit::PreviewDecisionKind;
  using playback_video_edit::SourceRange;
  using playback_video_edit::Timeline;

  bool ok = true;
  Timeline timeline(10'000'000);
  ok &= expect(timeline.isUnmodified(),
               "a new sequence must reference the complete source");
  ok &= expect(timeline.rippleDelete({3'000'000, 5'000'000}),
               "a middle range must be removable");
  ok &= expect(timeline.keptRanges() ==
                   std::vector<SourceRange>{{0, 3'000'000},
                                            {5'000'000, 10'000'000}},
               "middle removal must produce two source clips");
  ok &= expect(timeline.outputDurationUs() == 8'000'000,
               "ripple deletion must close the sequence gap");
  ok &= expect(timeline.sourceToOutputTime(5'500'000) ==
                   std::optional<int64_t>(3'500'000),
               "source-to-output projection must subtract deleted time");
  ok &= expect(!timeline.sourceToOutputTime(4'000'000),
               "deleted source time must not project into the sequence");
  ok &= expect(timeline.outputToSourceTime(3'500'000) ==
                   std::optional<int64_t>(5'500'000),
               "output-to-source projection must cross edit boundaries");
  ok &= expect(timeline.sourceToOutputTime(10'000'000) ==
                   std::optional<int64_t>(8'000'000),
               "the exclusive final source boundary must map to output end");
  ok &= expect(timeline.outputToSourceTime(8'000'000) ==
                   std::optional<int64_t>(10'000'000),
               "the exclusive output boundary must map to the final source end");
  ok &= expect(timeline.nextKeptSourceTime(4'000'000) ==
                   std::optional<int64_t>(5'000'000),
               "forward navigation must cross a removed source range");
  ok &= expect(timeline.previousKeptSourceTime(5'000'000) ==
                   std::optional<int64_t>(2'999'999),
               "backward navigation must land immediately before a cut");

  ok &= expect(timeline.rippleDelete({0, 1'000'000}),
               "the beginning must be trimmable through the same model");
  ok &= expect(timeline.rippleDelete({9'000'000, 10'000'000}),
               "the end must be trimmable through the same model");
  ok &= expect(timeline.keptRanges() ==
                   std::vector<SourceRange>{{1'000'000, 3'000'000},
                                            {5'000'000, 9'000'000}},
               "outer trims must preserve all remaining source clips");
  ok &= expect(!timeline.rippleDelete({0, 10'000'000}),
               "an edit must not publish an empty sequence");

  Timeline trimmed(10'000'000);
  ok &= expect(trimmed.rippleDelete({4'000'000, 6'000'000}),
               "trim setup must create an existing middle cut");
  ok &= expect(trimmed.trimTo({2'000'000, 8'000'000}),
               "trim-to-range must intersect existing edit decisions");
  ok &= expect(trimmed.keptRanges() ==
                   std::vector<SourceRange>{{2'000'000, 4'000'000},
                                            {6'000'000, 8'000'000}},
               "trim must never restore previously deleted media");

  EditSession session;
  session.activate(10'000'000);
  const uint64_t activeRevision = session.snapshot().revision;
  session.deactivate();
  ok &= expect(!session.snapshot().active &&
                   session.snapshot().revision == activeRevision + 1,
               "closing the editor must publish one observable revision");
  session.activate(10'000'000);
  ok &= expect(session.snapshot().active &&
                   session.snapshot().revision == activeRevision + 2,
               "reopening retained edits must publish one observable revision");
  session.markIn(3'000'000);
  session.markOut(5'000'000);
  ok &= expect(session.rippleDeleteSelection(),
               "a valid marked range must commit as one decision");
  ok &= expect(session.snapshot().canUndo && !session.snapshot().canRedo,
               "committing must create an undo revision");
  ok &= expect(session.undo() && session.timeline().isUnmodified(),
               "undo must restore the exact prior timeline snapshot");
  ok &= expect(session.redo() && session.timeline().outputDurationUs() ==
                                    8'000'000,
               "redo must restore the exact removed range");
  ok &= expect(session.resetEdits() && session.timeline().isUnmodified(),
               "reset must itself be an undoable timeline revision");
  ok &= expect(session.undo() && session.timeline().outputDurationUs() ==
                                    8'000'000,
               "reset must not discard edit history");

  session.markIn(1'000'000);
  session.markOut(2'000'000);
  ok &= expect(session.rippleDeleteSelection(),
               "a new decision after undo must commit normally");
  ok &= expect(!session.snapshot().canRedo,
               "a new decision must invalidate the abandoned redo branch");
  ok &= expect(session.undo(),
               "the branch-invalidation setup must remain undoable");

  session.markIn(2'000'000);
  session.markOut(8'000'000);
  ok &= expect(session.trimToSelection(),
               "explicit trim must share the marked-range interaction");
  ok &= expect(session.timeline().keptRanges() ==
                   std::vector<SourceRange>{{2'000'000, 3'000'000},
                                            {5'000'000, 8'000'000}},
               "trim must retain earlier middle deletions");

  const uint64_t markedRevision = session.snapshot().revision;
  session.markIn(2'000'000);
  ok &= expect(session.snapshot().revision == markedRevision + 1,
               "setting a changed mark must publish one revision");
  session.markIn(2'000'000);
  ok &= expect(session.snapshot().revision == markedRevision + 1,
               "setting an identical mark must not publish a false revision");

  PreviewController preview;
  int64_t previewStartUs = -1;
  ok &= expect(preview.start({{1'000'000, 2'000'000},
                              {4'000'000, 5'000'000}},
                             &previewStartUs) &&
                   previewStartUs == 1'000'000,
               "sequence preview must begin at the first retained clip");
  ok &= expect(preview.observePresentedFrame(1'500'000, 33'333, false).kind ==
                   PreviewDecisionKind::None,
               "preview must continue inside the current clip");
  ok &= expect(preview.observePresentedFrame(1'980'000, 33'333, false).kind ==
                       PreviewDecisionKind::Seek &&
                   preview.observePresentedFrame(1'980'000, 33'333, true).kind ==
                       PreviewDecisionKind::None,
               "preview must schedule one transition at the presented frame boundary");
  const auto finalPreviewDecision =
      preview.observePresentedFrame(4'980'000, 33'333, false);
  ok &= expect(finalPreviewDecision.kind == PreviewDecisionKind::Complete &&
                   !preview.active(),
               "preview must stop after the final retained frame interval");

  playback_video_edit::EditSnapshot overlayEdit;
  overlayEdit.active = true;
  overlayEdit.sourceDurationUs = 10'000'000;
  overlayEdit.outputDurationUs = 8'000'000;
  overlayEdit.keptRanges = {{0, 2'000'000}, {4'000'000, 10'000'000}};
  overlayEdit.inUs = 2'000'000;
  overlayEdit.outUs = 4'000'000;
  playback_video_edit::ExportSnapshot overlayExport;
  overlayExport.state = playback_video_edit::ExportState::Running;
  overlayExport.progress = 0.42;
  const playback_video_edit::OverlayModel overlayModel =
      playback_video_edit::buildOverlayModel(overlayEdit, &overlayExport, 10,
                                              0.5);
  ok &= expect(overlayModel.cells.size() == 10 &&
                   overlayModel.cells[0] ==
                       playback_video_edit::TimelineCellKind::Kept &&
                   overlayModel.cells[2] ==
                       playback_video_edit::TimelineCellKind::Selected &&
                   overlayModel.cells[4] ==
                       playback_video_edit::TimelineCellKind::Kept,
               "shared edit UI projection must distinguish kept and selected media");
  ok &= expect(overlayModel.inCell == std::optional<int>(2) &&
                   overlayModel.outCell == std::optional<int>(4) &&
                   overlayModel.playheadCell == 5,
               "shared edit UI projection must place marks and playhead consistently");
  ok &= expect(overlayModel.status.find("EXPORT 42%") != std::string::npos &&
                   overlayModel.status.find("Ctrl+E cancel") !=
                       std::string::npos,
               "shared edit UI projection must expose cancellable export progress");
  overlayEdit.inUs.reset();
  overlayEdit.outUs.reset();
  const playback_video_edit::OverlayModel unselectedOverlayModel =
      playback_video_edit::buildOverlayModel(overlayEdit, nullptr, 10, 0.0);
  ok &= expect(unselectedOverlayModel.cells[2] ==
                   playback_video_edit::TimelineCellKind::Removed &&
                   unselectedOverlayModel.status.find("Ctrl+E export") !=
                       std::string::npos,
               "shared edit UI projection must expose removed media and idle export action");
  overlayEdit.active = false;
  const playback_video_edit::OverlayModel backgroundExportModel =
      playback_video_edit::buildOverlayModel(overlayEdit, &overlayExport, 10,
                                              0.0);
  ok &= expect(backgroundExportModel.cells.empty() &&
                   backgroundExportModel.status.find("EXPORT 42%") !=
                       std::string::npos &&
                   backgroundExportModel.status.find("Ctrl+E cancel") !=
                       std::string::npos,
               "background export progress must remain visible after the editor closes");

  return ok ? 0 : 1;
}
