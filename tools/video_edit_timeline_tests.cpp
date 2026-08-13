#include "playback/video/edit/timeline.h"
#include "playback/video/edit/overlay_model.h"
#include "playback/overlay/context_menu.h"
#include "playback/overlay/interaction.h"
#include "playback/session/context_menu_controller.h"

#include <algorithm>
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
  using playback_video_edit::Document;
  using playback_video_edit::ExitPrompt;
  using playback_video_edit::Selection;
  using playback_video_edit::SourceRange;
  using playback_video_edit::Timeline;
  using SequenceTimeline = playback_video_sequence::Timeline;

  bool ok = true;
  const Timeline unopenedTimeline;
  ok &= expect(unopenedTimeline.isUnmodified(),
               "an unopened edit document must not be dirty");
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
  const auto sequence =
      SequenceTimeline::create(timeline.sourceDurationUs(),
                               timeline.keptRanges());
  ok &= expect(sequence && sequence->durationUs() == 8'000'000,
               "the playback sequence must derive the ripple duration");
  if (sequence) {
    const playback_video_sequence::Point afterCut =
        sequence->pointAt(3'000'000);
    ok &= expect(afterCut.clipIndex == 1 &&
                     afterCut.sourceUs == 5'000'000 &&
                     afterCut.presentationUs == 3'000'000,
                 "presentation time must cross a cut without a time gap");
    const auto removedForward = sequence->pointForSource(
        4'000'000, playback_video_sequence::SourceBias::Forward);
    const auto removedBackward = sequence->pointForSource(
        4'000'000, playback_video_sequence::SourceBias::Backward);
    ok &= expect(removedForward && removedForward->sourceUs == 5'000'000 &&
                     removedForward->presentationUs == 3'000'000,
                 "a removed source point must resolve to the next clip");
    ok &= expect(removedBackward && removedBackward->sourceUs == 3'000'000 &&
                     removedBackward->presentationUs == 3'000'000,
                 "backward source resolution must select the previous cut edge");
    ok &= expect(sequence->clipIndexAtSource(2'000'000) ==
                         std::optional<size_t>(0) &&
                     !sequence->clipIndexAtSource(4'000'000) &&
                     sequence->clipIndexAtSource(5'000'000) ==
                         std::optional<size_t>(1),
                 "source lookup must distinguish kept clips from removed gaps");
    const auto mappedFrame = sequence->mapFrame(2'900'000, 200'000);
    ok &= expect(mappedFrame &&
                     mappedFrame->presentationPtsUs == 2'900'000 &&
                     mappedFrame->presentationDurationUs == 100'000 &&
                     !sequence->mapFrame(4'000'000, 100'000),
                 "video projection must trim at clip out-points and reject "
                 "removed source frames");
    const auto audioSlice =
        sequence->sliceAudio(4'500'000, 1000, 1000);
    ok &= expect(audioSlice && audioSlice->sourceOffsetFrames == 500 &&
                     audioSlice->frameCount == 500 &&
                     audioSlice->presentationPtsUs == 3'000'000,
                 "audio projection must slice at clip in-points and map onto "
                 "the ripple timeline");
  }
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

  Document document;
  document.load(10'000'000);
  Selection selection;
  ok &= expect(!playback_video_edit::buildSnapshot(document, selection, false)
                    .active &&
                   playback_video_edit::buildSnapshot(document, selection,
                                                        true)
                       .active,
               "workspace activation must be projected without changing the document");
  Selection clearableSelection;
  clearableSelection.markIn(document.timeline(), 1'000'000);
  clearableSelection.markOut(document.timeline(), 2'000'000);
  ok &= expect(
      clearableSelection.hasMarks() &&
          clearableSelection.clear(playback_video_edit::EditBoundary::In) &&
          !clearableSelection.inSourceUs() &&
          clearableSelection.outSourceUs() ==
              std::optional<int64_t>(2'000'000),
      "an active In control must clear only its own mark");
  ok &= expect(clearableSelection.clear() &&
                   !clearableSelection.inSourceUs() &&
                   !clearableSelection.outSourceUs() &&
                   !clearableSelection.hasMarks() &&
                   !clearableSelection.clear(),
               "Escape-style selection cancellation must clear all marks once");
  selection.markIn(document.timeline(), 3'000'000);
  selection.markOut(document.timeline(), 5'000'000);
  const auto initialRemoval = selection.range();
  ok &= expect(initialRemoval && document.rippleDelete(*initialRemoval),
               "a valid marked range must commit as one decision");
  selection.clear();
  ok &= expect(document.undo() && document.timeline().isUnmodified(),
               "undo must restore the exact prior timeline snapshot");
  ok &= expect(document.redo() && document.timeline().outputDurationUs() ==
                                     8'000'000,
               "redo must restore the exact removed range");
  ok &= expect(document.resetEdits() && document.timeline().isUnmodified(),
               "reset must itself be an undoable timeline revision");
  ok &= expect(document.undo() && document.timeline().outputDurationUs() ==
                                     8'000'000,
               "reset must not discard edit history");

  Document discarded;
  discarded.load(10'000'000);
  ok &= expect(discarded.rippleDelete({3'000'000, 5'000'000}) &&
                   playback_video_edit::buildSnapshot(
                       discarded, Selection{}, true)
                       .hasEdits &&
                   playback_video_edit::buildSnapshot(
                       discarded, Selection{}, true)
                       .hasUnexportedChanges,
               "committed edit decisions must expose edit and dirty state");
  ok &= expect(discarded.discardAllChanges() &&
                   discarded.timeline().isUnmodified() &&
                   !playback_video_edit::buildSnapshot(
                        discarded, Selection{}, true)
                        .hasEdits &&
                   !discarded.canUndo() &&
                   !discarded.canRedo(),
               "discard must restore the source sequence and clear its history");

  Document exported;
  exported.load(10'000'000);
  ok &= expect(exported.rippleDelete({2'000'000, 3'000'000}) &&
                   exported.hasUnexportedChanges(),
               "a committed decision must make its document dirty");
  const std::vector<SourceRange> exportedRevision =
      exported.timeline().keptRanges();
  exported.markExported(exportedRevision);
  const auto exportedSnapshot = playback_video_edit::buildSnapshot(
      exported, Selection{}, true);
  ok &= expect(exportedSnapshot.hasEdits &&
                   !exportedSnapshot.hasUnexportedChanges,
               "a successful export must mark its exact document revision clean");
  ok &= expect(exported.rippleDelete({6'000'000, 7'000'000}) &&
                   exported.hasUnexportedChanges(),
               "editing after export must create a new dirty revision");
  ok &= expect(exported.undo() &&
                   exported.timeline().keptRanges() == exportedRevision &&
                   !exported.hasUnexportedChanges(),
               "undoing to the exported revision must restore clean state");
  ok &= expect(exported.redo() && exported.hasUnexportedChanges(),
               "redoing past the exported revision must restore dirty state");

  Document asynchronousExport;
  asynchronousExport.load(10'000'000);
  ok &= expect(asynchronousExport.rippleDelete({2'000'000, 3'000'000}),
               "asynchronous export setup must create its first revision");
  const std::vector<SourceRange> queuedExportRevision =
      asynchronousExport.timeline().keptRanges();
  ok &= expect(asynchronousExport.rippleDelete({6'000'000, 7'000'000}),
               "editing may continue while an older revision exports");
  asynchronousExport.markExported(queuedExportRevision);
  ok &= expect(asynchronousExport.hasUnexportedChanges(),
               "finishing an older export must not mark newer decisions clean");
  ok &= expect(asynchronousExport.undo() &&
                   !asynchronousExport.hasUnexportedChanges(),
               "the exported asynchronous revision must remain the clean baseline");

  ok &= expect(document.rippleDelete({1'000'000, 2'000'000}),
               "a new decision after undo must commit normally");
  ok &= expect(!document.redo(),
               "a new decision must invalidate the abandoned redo branch");
  ok &= expect(document.undo(),
               "the branch-invalidation setup must remain undoable");

  ok &= expect(document.trimTo({2'000'000, 8'000'000}),
               "explicit trim must share the marked-range interaction");
  ok &= expect(document.timeline().keptRanges() ==
                   std::vector<SourceRange>{{2'000'000, 3'000'000},
                                            {5'000'000, 8'000'000}},
               "trim must retain earlier middle deletions");

  selection.markIn(document.timeline(), 2'000'000);
  ok &= expect(playback_video_edit::buildSnapshot(document, selection, true)
                       .inSourceUs ==
                   std::optional<int64_t>(2'000'000),
                "setting an In mark must publish its source position");
  selection.markIn(document.timeline(), 2'000'000);
  ok &= expect(playback_video_edit::buildSnapshot(document, selection, true)
                       .inSourceUs ==
                   std::optional<int64_t>(2'000'000),
                "setting an identical mark must preserve the selection");

  Document draggedDocument;
  draggedDocument.load(10'000'000);
  ok &= expect(draggedDocument.rippleDelete({2'000'000, 4'000'000}),
               "drag mapping setup must create a source gap");
  Selection draggedIn;
  ok &= expect(draggedIn.moveBoundary(
                   draggedDocument.timeline(),
                   playback_video_edit::EditBoundary::In, 2'000'000,
                   100'000) &&
                   draggedIn.inSourceUs() ==
                       std::optional<int64_t>(4'000'000),
               "an In handle at a cut must bind to the following clip");

  Selection draggedOut;
  ok &= expect(draggedOut.moveBoundary(
                   draggedDocument.timeline(),
                   playback_video_edit::EditBoundary::Out, 2'000'000,
                   100'000) &&
                   draggedOut.outSourceUs() ==
                       std::optional<int64_t>(2'000'000),
               "an Out handle at a cut must bind to the preceding clip edge");
  draggedOut.markIn(draggedDocument.timeline(), 1'000'000);
  ok &= expect(draggedOut.moveBoundary(
                   draggedDocument.timeline(),
                   playback_video_edit::EditBoundary::In, 3'000'000,
                   100'000) &&
                   playback_video_edit::buildSnapshot(
                       draggedDocument, draggedOut, true)
                           .inTimelineUs ==
                       std::optional<int64_t>(1'900'000) &&
                   playback_video_edit::buildSnapshot(
                       draggedDocument, draggedOut, true)
                           .outTimelineUs ==
                       std::optional<int64_t>(2'000'000),
               "dragged boundaries must clamp instead of crossing");

  playback_video_edit::EditSnapshot overlayEdit;
  overlayEdit.active = true;
  overlayEdit.hasUnexportedChanges = true;
  overlayEdit.sourceDurationUs = 10'000'000;
  overlayEdit.timelineDurationUs = 8'000'000;
  overlayEdit.frameDurationUs = 33'333;
  overlayEdit.keptRanges = {{0, 2'000'000}, {4'000'000, 10'000'000}};
  overlayEdit.clips = {{{0, 2'000'000}, 0},
                       {{4'000'000, 10'000'000}, 2'000'000}};
  overlayEdit.inTimelineUs = 2'000'000;
  overlayEdit.outTimelineUs = 4'000'000;
  overlayEdit.playheadTimelineUs = 4'000'000;
  playback_video_edit::ExportProgress overlayExport;
  overlayExport.active = true;
  overlayExport.fraction = 0.42;
  const playback_video_edit::OverlayModel overlayModel =
      playback_video_edit::buildOverlayModel(overlayEdit, &overlayExport,
                                              ExitPrompt::None, 10, 0.5);
  ok &= expect(overlayModel.cells.size() == 10 &&
                    overlayModel.cells[0] ==
                        playback_video_edit::TimelineCellKind::Kept &&
                    overlayModel.cells[2] ==
                        playback_video_edit::TimelineCellKind::Selected &&
                    overlayModel.cells[4] ==
                        playback_video_edit::TimelineCellKind::Selected,
                "the edit UI must project selection in compact program time");
  ok &= expect(overlayModel.inCell == std::optional<int>(2) &&
                    overlayModel.outCell == std::optional<int>(5) &&
                    overlayModel.playheadCell == 5 &&
                    overlayModel.cutCells == std::vector<int>{2},
                "marks, cuts, and playhead must share the program-time axis");
  ok &= expect(overlayModel.status == "EDIT MODE*" &&
                    overlayModel.status.size() <= 10,
               "narrow editor status must keep the active mode visible");
  const playback_video_edit::OverlayModel tinyExportModel =
      playback_video_edit::buildOverlayModel(overlayEdit, &overlayExport,
                                              ExitPrompt::None, 6, 0.5);
  ok &= expect(tinyExportModel.status == "EDIT*",
               "tiny editor status must retain a compact mode indicator");
  const playback_video_edit::OverlayModel tinyDirtyModel =
      playback_video_edit::buildOverlayModel(
          overlayEdit, nullptr, ExitPrompt::None, 1, 0.5);
  ok &= expect(tinyDirtyModel.status == "*",
               "one-column editor status must retain the dirty indicator");
  const playback_video_edit::OverlayModel wideOverlayModel =
      playback_video_edit::buildOverlayModel(overlayEdit, &overlayExport,
                                              ExitPrompt::None, 96, 0.5);
  ok &= expect(wideOverlayModel.status.find("EXPORT 42%") !=
                        std::string::npos &&
                    wideOverlayModel.status.find("TC 00:00:04:00") !=
                        std::string::npos &&
                    wideOverlayModel.status.find("EDIT MODE*") !=
                        std::string::npos &&
                    wideOverlayModel.status.find("Ctrl+") ==
                        std::string::npos &&
                    wideOverlayModel.status.size() <= 96,
               "wide editor status must add state without duplicating controls");
  overlayEdit.inTimelineUs.reset();
  overlayEdit.outTimelineUs.reset();
  const playback_video_edit::OverlayModel unselectedOverlayModel =
      playback_video_edit::buildOverlayModel(
          overlayEdit, nullptr, ExitPrompt::None, 10, 0.0);
  ok &= expect(unselectedOverlayModel.cells[2] ==
                   playback_video_edit::TimelineCellKind::Kept &&
                   unselectedOverlayModel.cutCells == std::vector<int>{2},
                "removed source gaps must collapse to explicit cut points");
  overlayEdit.active = false;
  const playback_video_edit::OverlayModel backgroundExportModel =
      playback_video_edit::buildOverlayModel(overlayEdit, &overlayExport,
                                              ExitPrompt::None, 10, 0.0);
  ok &= expect(backgroundExportModel.cells.empty() &&
                    backgroundExportModel.status.find("EXPORT 42%") !=
                        std::string::npos,
                "background export progress must remain visible after the editor closes");

  const playback_video_edit::OverlayModel exitModel =
      playback_video_edit::buildOverlayModel(
          overlayEdit, nullptr, ExitPrompt::LeavePlayback, 10, 0.0);
  ok &= expect(exitModel.status == "UNEXPORTED" &&
                   exitModel.status.size() <= 10,
               "exit confirmation must use a complete width-bounded state");

  const playback_video_edit::OverlayModel closeEditorModel =
      playback_video_edit::buildOverlayModel(
          overlayEdit, nullptr, ExitPrompt::CloseEditor, 16, 0.0);
  ok &= expect(closeEditorModel.status == "LEAVE EDIT MODE?",
               "leaving only edit mode must have its own explicit prompt");

  playback_overlay::InteractionMap interactions;
  interactions.progressBar =
      playback_overlay::ProgressBarRegion{{100.0, 200.0, 200.0, 210.0}, 10};
  interactions.controls.push_back(
      {{20.0, 30.0, 40.0, 40.0},
       playback_overlay::OverlayControlId::EditExport});
  interactions.editBoundaries.push_back(
      {{95.0, 200.0, 115.0, 210.0},
       playback_video_edit::EditBoundary::In});
  auto progressHit =
      playback_overlay::progressBarHitAt(interactions, 149.5, 205.0);
  ok &= expect(progressHit && progressHit->ratio == 0.5 &&
                   progressHit->units == 10,
               "progress hit-testing must preserve exact in-bar geometry");
  ok &= expect(!playback_overlay::progressBarHitAt(interactions, 200.0,
                                                    205.0),
               "ordinary progress hit-testing must reject outside input");
  progressHit =
      playback_overlay::progressBarHitAt(interactions, 0.0, 0.0, true);
  ok &= expect(progressHit && progressHit->ratio == 0.0,
               "captured progress drags must clamp before the bar");
  progressHit =
      playback_overlay::progressBarHitAt(interactions, 500.0, 500.0, true);
  ok &= expect(progressHit && progressHit->ratio == 1.0,
               "captured progress drags must clamp beyond the bar");
  ok &= expect(playback_overlay::overlayControlAt(interactions, 25.0, 35.0) ==
                   playback_overlay::OverlayControlId::EditExport,
               "rendered controls must retain their semantic identity");
  ok &= expect(playback_overlay::editBoundaryAt(interactions, 100.0, 205.0) ==
                   playback_video_edit::EditBoundary::In,
               "rendered edit handles must retain their boundary identity");
  const playback_overlay::InteractionMap transformed =
      playback_overlay::transformInteractionMap(interactions, 5.0, 7.0,
                                                2.0, 3.0);
  ok &= expect(playback_overlay::overlayControlAt(transformed, 50.0, 100.0) ==
                   playback_overlay::OverlayControlId::EditExport,
               "presentation transforms must preserve exact control hits");
  const playback_overlay::InteractionHit transformedHit =
      playback_overlay::interactionHitAtTransformed(
          interactions, 5.0, 7.0, 2.0, 3.0, 304.5, 610.0);
  ok &= expect(transformedHit.progressBar &&
                   transformedHit.progressBar->ratio == 0.5,
               "pixel mouse input must preserve sub-cell progress precision");

  playback_overlay::ContextMenuSnapshot contextMenu;
  contextMenu.visible = true;
  contextMenu.anchorXRatio = 1.0;
  contextMenu.anchorYRatio = 1.0;
  contextMenu.selectedItem = 20;
  contextMenu.items = {
      {10, "Edit video"},
      {20, "Discard changes"},
  };
  const playback_overlay::ContextMenuCellLayout contextLayout =
      playback_overlay::layoutContextMenuCells(contextMenu, 30, 10);
  ok &= expect(contextLayout.drawable() && contextLayout.x >= 0 &&
                   contextLayout.y >= 0 &&
                   contextLayout.x + contextLayout.width <= 30 &&
                   contextLayout.y + contextLayout.height <= 10,
               "a context menu must flip and clamp inside its presentation surface");
  ok &= expect(contextLayout.items.size() == 2 &&
                   !contextLayout.items[0].selected &&
                   contextLayout.items[1].selected,
               "context menu selection must retain semantic command identity");
  const playback_overlay::InteractionMap contextInteractions =
      playback_overlay::buildContextMenuInteractionMap(contextLayout);
  ok &= expect(contextInteractions.modal &&
                   contextInteractions.contains(0.0, 0.0),
               "a visible context menu must own input outside its popup");
  ok &= expect(playback_overlay::contextMenuItemAt(
                   contextInteractions,
                   static_cast<double>(contextLayout.items[1].x),
                   static_cast<double>(contextLayout.items[1].y)) == 20,
               "context menu hit-testing must preserve its opaque item token");
  const auto transformedMenuHit =
      playback_overlay::interactionHitAtTransformed(
          contextInteractions, 5.0, 7.0, 2.0, 3.0,
          5.0 + (static_cast<double>(contextLayout.items[1].x) + 0.5) * 2.0,
          7.0 + (static_cast<double>(contextLayout.items[1].y) + 0.5) * 3.0);
  ok &= expect(transformedMenuHit.contextMenuItem == 20,
               "framebuffer scaling must preserve context-menu item identity");

  playback_session::ContextMenuController playbackMenu;
  playback_video_edit::EditSnapshot cleanEdit;
  playback_video_edit::ExportProgress idleExport;
  playbackMenu.refresh(cleanEdit, idleExport);
  ok &= expect(playbackMenu.open(
                   playback_session::ContextMenuSurface::Terminal, 0.25, 0.75),
               "ordinary playback must expose an explicit edit command");
  const auto terminalMenu = playbackMenu.snapshotFor(
      playback_session::ContextMenuSurface::Terminal);
  const auto windowMenu = playbackMenu.snapshotFor(
      playback_session::ContextMenuSurface::VideoWindow);
  ok &= expect(terminalMenu.visible && terminalMenu.items.size() == 1 &&
                   terminalMenu.items[0].label == "Edit video" &&
                   !windowMenu.visible,
               "a playback context menu must belong to exactly one presentation surface");
  cleanEdit.hasEdits = true;
  playbackMenu.refresh(cleanEdit, idleExport);
  const auto retainedMenu = playbackMenu.snapshotFor(
      playback_session::ContextMenuSurface::Terminal);
  ok &= expect(retainedMenu.items.size() == 1 &&
                   retainedMenu.items[0].label == "Resume editing",
               "rendered edits without newer changes must remain resumable without save actions");
  cleanEdit.active = true;
  cleanEdit.hasUnexportedChanges = true;
  cleanEdit.inTimelineUs = 1'000'000;
  cleanEdit.outTimelineUs = 2'000'000;
  playbackMenu.refresh(cleanEdit, idleExport);
  const auto dirtyMenu = playbackMenu.snapshotFor(
      playback_session::ContextMenuSurface::Terminal);
  const auto clearAllItem = std::find_if(
      dirtyMenu.items.begin(), dirtyMenu.items.end(), [](const auto& item) {
        return item.label == "Clear In and Out";
      });
  const auto leaveItem = std::find_if(
      dirtyMenu.items.begin(), dirtyMenu.items.end(), [](const auto& item) {
        return item.label == "Leave edit mode";
      });
  const auto discardItem = std::find_if(
      dirtyMenu.items.begin(), dirtyMenu.items.end(), [](const auto& item) {
        return item.label == "Discard changes";
      });
  ok &= expect(clearAllItem != dirtyMenu.items.end() &&
                   leaveItem != dirtyMenu.items.end() &&
                   discardItem != dirtyMenu.items.end() &&
                   playbackMenu.select(discardItem->token) &&
                   playbackMenu.activateSelection() ==
                       playback_video_edit::Command::Discard &&
                   !playbackMenu.visible(),
               "context commands must expose clear, leave, and document actions");

  return ok ? 0 : 1;
}
