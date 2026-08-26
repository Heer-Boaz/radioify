#include "playback/video/edit/command.h"
#include "playback/video/edit/overlay_model.h"
#include "playback/video/edit/timeline.h"
#include "playback/video/composition/render_plan.h"
#include "playback/video/frame_step_prefetch.h"
#include "playback/overlay/context_menu.h"
#include "playback/overlay/overlay.h"
#include "playback/session/context_menu_controller.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "video_edit_timeline_tests: " << message << '\n';
    return false;
  }
  return true;
}

std::vector<playback_video_composition::SourceFrameTiming> observedTiming(
    const std::vector<int64_t>& pts, int64_t quantumUs = 1) {
  const auto quantize = [quantumUs](int64_t value) {
    return ((value + quantumUs / 2) / quantumUs) * quantumUs;
  };
  std::vector<playback_video_composition::SourceFrameTiming> frames;
  frames.reserve(pts.size());
  for (size_t index = 0; index < pts.size(); ++index) {
    const int64_t framePtsUs = quantize(pts[index]);
    const int64_t durationUs =
        index + 1 < pts.size() ? quantize(pts[index + 1]) - framePtsUs : 0;
    frames.push_back({framePtsUs, durationUs});
  }
  return frames;
}

}  // namespace

int main() {
  using playback_video_edit::Document;
  using playback_video_edit::DecisionList;
  using playback_video_edit::ExitExportAction;
  using playback_video_edit::ExitContext;
  using playback_video_edit::Prompt;
  using playback_video_edit::Selection;
  using playback_video_edit::SourceRange;
  using playback_video_edit::Timeline;
  using SequenceTimeline = playback_video_sequence::Timeline;

  bool ok = true;
  ok &= expect(
      playback_video_edit::CutTransition::motionSmooth(1).durationFrames() ==
              playback_video_edit::kMinimumSmoothCutFrames &&
          playback_video_edit::CutTransition::motionSmooth(255)
                  .durationFrames() ==
              playback_video_edit::kMaximumSmoothCutFrames,
      "smooth-cut duration must stay inside the editor's bounded handle "
      "window");
  playback_video_composition::RenderPlan invalidTransitionPlan;
  std::string invalidTransitionError;
  const playback_video_composition::SourceTiming timing30{
      AVRational{30, 1}, AVRational{1, 90'000}};
  const playback_video_composition::SourceTiming timingNtsc{
      AVRational{30'000, 1001}, AVRational{1, 90'000}};
  ok &= expect(
      !playback_video_composition::buildRenderPlan(
          {{0, 1'000'000}, {2'000'000, 3'000'000}},
          {{playback_video_sequence::TransitionKind::MotionSmooth, 255, 7}},
          timing30, &invalidTransitionPlan, &invalidTransitionError),
      "transition handle validation must not wrap at eight bits");
  playback_video_composition::RenderPlan motionPlan;
  std::string motionPlanError;
  const std::vector<SourceRange> motionRanges{{0, 3'003'000},
                                               {5'005'000, 10'010'000}};
  const std::vector<playback_video_edit::CutTransition> motionTransitions{
      playback_video_edit::CutTransition::motionSmooth()};
  ok &= expect(playback_video_composition::buildRenderPlan(
                   motionRanges, motionTransitions, timingNtsc,
                   &motionPlan, &motionPlanError) &&
                   motionPlan.hasMotionTransitions() &&
                   motionPlan.motionTransitions.size() == 1 &&
                   motionPlan.motionTransitions[0].outgoingFrames == 2 &&
                   motionPlan.motionTransitions[0].incomingFrames == 2 &&
                   motionPlan.motionTransitions[0].durationFrames == 4 &&
                   motionPlan.clips[0].sourceEndUs ==
                       motionPlan.motionTransitions[0].outgoingAnchorUs &&
                   motionPlan.clips[1].sourceStartUs ==
                       motionPlan.motionTransitions[0].incomingAnchorUs,
               "a motion transition must own explicit coterminous outgoing "
               "and incoming overlap windows");
  if (!motionPlan.motionTransitions.empty()) {
    const auto& window = motionPlan.motionTransitions[0];
    const int64_t renderedDurationUs =
        motionPlan.clips[0].sourceEndUs -
            motionPlan.clips[0].sourceStartUs +
        window.durationUs + motionPlan.clips[1].sourceEndUs -
            motionPlan.clips[1].sourceStartUs;
    const std::string transitionFilter =
        playback_video_composition::buildTransitionFilterDescription(
            motionPlan, window, AV_PIX_FMT_P010LE, &motionPlanError);
    const std::string programFilter =
        playback_video_composition::buildProgramFilterDescription(
            motionPlan, AV_PIX_FMT_P010LE, &motionPlanError);
    const std::string exactCore =
        "settb=1001/30000,setpts=N+gte(N\\,2)*3";
    ok &= expect(std::llabs(renderedDurationUs - 8'008'000) <= 1 &&
                     window.presentationStartUs ==
                         motionPlan.clips[0].sourceEndUs &&
                     transitionFilter.find(exactCore) != std::string::npos &&
                     programFilter.find(exactCore) != std::string::npos &&
                     transitionFilter.find(
                         "trim=start_frame=1:end_frame=5") !=
                         std::string::npos &&
                     transitionFilter.find(
                         "format=pix_fmts=yuv420p10le,settb=1001/30000") !=
                         std::string::npos &&
                     programFilter.find("[raw_m0]trim=start_pts=") !=
                         std::string::npos &&
                     programFilter.find("[raw_l0]trim=start_pts=") !=
                         std::string::npos &&
                     transitionFilter.find("split=2") == std::string::npos,
                 "preview and export must share exact frame-tick motion "
                 "evaluation over source-PTS clip windows");

    playback_video_composition::MotionSourceTiming expectedTiming;
    std::string cadenceError;
    const bool resolvedTiming =
        playback_video_composition::motionSourceTiming(
            motionPlan, window, &expectedTiming, &cadenceError);
    auto outgoingTiming = observedTiming(expectedTiming.outgoingPtsUs);
    auto incomingTiming = observedTiming(expectedTiming.incomingPtsUs);
    ok &= expect(
        resolvedTiming &&
            playback_video_composition::validateMotionSourceTiming(
                motionPlan, window, outgoingTiming, incomingTiming,
                &cadenceError),
        "a motion transition must accept its exact bounded source cadence");
    if (incomingTiming.size() > 1) {
      incomingTiming[1].ptsUs += motionPlan.frameDurationUs / 2;
    }
    cadenceError.clear();
    ok &= expect(
        !playback_video_composition::validateMotionSourceTiming(
            motionPlan, window, outgoingTiming, incomingTiming,
            &cadenceError) &&
            cadenceError.find("hard cut") != std::string::npos,
        "a locally variable source cadence must fail closed before motion "
        "rendering");
  }
  playback_video_composition::RenderPlan millisecondPlan;
  std::string millisecondError;
  ok &= expect(
      playback_video_composition::buildRenderPlan(
          {{200'000, 1'233'000}, {1'800'000, 2'800'000}},
          motionTransitions,
          {AVRational{30, 1}, AVRational{1, 1'000}}, &millisecondPlan,
          &millisecondError),
      "a millisecond source clock must remain usable for 30 fps video");
  if (millisecondPlan.hasMotionTransitions()) {
    const auto& window = millisecondPlan.motionTransitions.front();
    playback_video_composition::MotionSourceTiming expectedTiming;
    ok &= expect(playback_video_composition::motionSourceTiming(
                     millisecondPlan, window, &expectedTiming,
                     &millisecondError),
                 "millisecond source timing must resolve");
    ok &= expect(
        playback_video_composition::validateMotionSourceTiming(
            millisecondPlan, window,
            observedTiming(expectedTiming.outgoingPtsUs, 1'000),
            observedTiming(expectedTiming.incomingPtsUs, 1'000),
            &millisecondError),
        "stable CFR timing quantized to millisecond ticks must not be "
        "misclassified as VFR");
  }
  playback_video_composition::RenderPlan phasePlan;
  ok &= expect(
      playback_video_composition::buildRenderPlan(
          {{1'000, 3'004'000}, {5'006'000, 10'011'000}},
          motionTransitions, timingNtsc, &phasePlan, &motionPlanError) &&
          phasePlan.motionTransitions[0].outgoingAnchorUs ==
              3'004'000 - 66'733 &&
          phasePlan.motionTransitions[0].incomingAnchorUs ==
              5'006'000 + 66'733,
      "source-frame windows must remain relative to real edit boundaries, "
      "not a synthetic zero-based frame grid");
  playback_video_composition::RenderPlan coarseTimingPlan;
  ok &= expect(
      !playback_video_composition::buildRenderPlan(
          {{0, 1'000'000}, {2'000'000, 3'000'000}}, motionTransitions,
          {AVRational{30, 1}, AVRational{1, 25}}, &coarseTimingPlan,
          &motionPlanError),
      "smooth cuts must reject a source clock that cannot identify frames");
  playback_video_composition::RenderPlan exactFrameClockPlan;
  ok &= expect(
      playback_video_composition::buildRenderPlan(
          {{0, 1'000'000}, {2'000'000, 3'000'000}}, motionTransitions,
          {AVRational{30, 1}, AVRational{1, 30}}, &exactFrameClockPlan,
          &motionPlanError),
      "a coarse clock must remain valid when each tick is exactly one frame");
  playback_video_composition::RenderPlan overlappingPlan;
  ok &= expect(
      !playback_video_composition::buildRenderPlan(
          {{0, 200'000}, {300'000, 500'000}, {600'000, 800'000}},
          {playback_video_edit::CutTransition::motionSmooth(6),
           playback_video_edit::CutTransition::motionSmooth(6)},
          timing30, &overlappingPlan, &motionPlanError),
      "two transition overlaps must never consume the same clip interval");
  playback_video_composition::RenderPlan sparseMotionPlan;
  ok &= expect(
      playback_video_composition::buildRenderPlan(
          {{0, 2'000'000}, {3'000'000, 5'000'000},
           {6'000'000, 8'000'000}, {9'000'000, 11'000'000}},
          {playback_video_edit::CutTransition::motionSmooth(),
           playback_video_edit::CutTransition::hard(),
           playback_video_edit::CutTransition::motionSmooth()},
          timing30, &sparseMotionPlan, &motionPlanError) &&
          sparseMotionPlan.motionTransitions.size() == 2 &&
          sparseMotionPlan.motionTransitions[0].cutIndex == 0 &&
          sparseMotionPlan.motionTransitions[1].cutIndex == 2 &&
          sparseMotionPlan.motionTransitionIndexNear(2'500'000, 500'000) ==
              std::optional<size_t>{0} &&
          !sparseMotionPlan.motionTransitionIndexNear(4'000'000, 500'000) &&
          sparseMotionPlan.motionTransitionIndexNear(5'000'000, 1'000'000) ==
              std::optional<size_t>{1},
      "the immutable render plan must index nearby executable transitions "
      "without scanning hard cuts");
  const auto mappedSequence = SequenceTimeline::create(
      4'000'000, {{500'000, 1'500'000}, {2'500'000, 3'500'000}});
  playback_video_frame_step_prefetch::Request roundedFrameRequest;
  roundedFrameRequest.serial = 1;
  roundedFrameRequest.direction = playback_video_frame_step::Direction::Next;
  roundedFrameRequest.boundary.ptsUs = 916'667;
  roundedFrameRequest.boundary.sourcePtsUs = 1'416'667;
  roundedFrameRequest.boundary.durationUs = 41'667;
  roundedFrameRequest.join = roundedFrameRequest.boundary;
  roundedFrameRequest.rangeStartUs = 916'667;
  roundedFrameRequest.rangeEndUs = 1'958'334;
  const auto roundedFrameMapping =
      mappedSequence
          ? playback_video_frame_step_prefetch::mapRequestToTimeline(
                *mappedSequence, roundedFrameRequest)
          : std::nullopt;
  ok &= expect(
      roundedFrameMapping &&
          roundedFrameMapping->joinContinuity ==
              playback_video_frame_step_prefetch::JoinContinuity::Source &&
          roundedFrameMapping->sourceRangeStartUs == 1'416'667 &&
          roundedFrameMapping->sourceRangeEndUs == 1'500'000,
      "24 fps rounding must not cross a clip boundary before the current "
      "frame reaches it");
  roundedFrameRequest.boundary.ptsUs = 958'333;
  roundedFrameRequest.boundary.sourcePtsUs = 1'458'333;
  roundedFrameRequest.join = roundedFrameRequest.boundary;
  roundedFrameRequest.rangeStartUs = 958'333;
  const auto boundaryFrameMapping =
      mappedSequence
          ? playback_video_frame_step_prefetch::mapRequestToTimeline(
                *mappedSequence, roundedFrameRequest)
          : std::nullopt;
  ok &= expect(
      boundaryFrameMapping &&
          boundaryFrameMapping->joinContinuity ==
              playback_video_frame_step_prefetch::JoinContinuity::Presentation &&
          boundaryFrameMapping->sourceRangeStartUs == 2'500'000,
      "the final frame must cross to the next clip on its exact half-open "
      "boundary");
  ok &= expect(playback_video_edit::exitExportAction(ExitContext{}) ==
                   ExitExportAction::None &&
                   playback_video_edit::exitExportAction(
                       ExitContext{true, false, false}) ==
                       ExitExportAction::ExportCurrent &&
                   playback_video_edit::exitExportAction(
                       ExitContext{true, true, true}) ==
                       ExitExportAction::WaitForExport &&
                   playback_video_edit::exitExportAction(
                       ExitContext{false, true, false}) ==
                       ExitExportAction::WaitForExport &&
                   playback_video_edit::exitExportAction(
                       ExitContext{true, true, false}) ==
                       ExitExportAction::CancelBlockingExport,
               "playback exit must distinguish exporting, waiting, and a "
               "blocking older export from leaving the edit tools");
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
  ok &= expect(timeline.cutTransitions() ==
                   std::vector<playback_video_edit::CutTransition>{
                       playback_video_edit::CutTransition::hard()} &&
                   timeline.nearestCutIndex(3'000'000, 1) ==
                       std::optional<size_t>(0) &&
                   timeline.setCutTransition(
                       0, playback_video_edit::CutTransition::motionSmooth()) &&
                   timeline.cutTransitions()[0] ==
                       playback_video_edit::CutTransition::motionSmooth(),
               "a cut must own its explicit hard/smooth transition");
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
    const playback_video_sequence::Point playableEnd =
        sequence->pointAtPlaybackPosition(sequence->durationUs());
    ok &= expect(playableEnd.clipIndex == 1 &&
                     playableEnd.sourceUs == 9'999'999 &&
                     playableEnd.presentationUs == 7'999'999,
                 "a sequence-end edit point must resolve inside the final "
                 "frame interval for playback");
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
  ok &= expect(timeline.cutTransitions()[0] ==
                   playback_video_edit::CutTransition::motionSmooth(),
               "trimming clip edges must preserve an unchanged edit point");
  ok &= expect(timeline.rippleDelete({6'000'000, 7'000'000}) &&
                   timeline.keptRanges() ==
                       std::vector<SourceRange>{{1'000'000, 3'000'000},
                                                {5'000'000, 6'000'000},
                                                {7'000'000, 9'000'000}} &&
                   timeline.cutTransitions().size() == 2,
               "a second non-adjacent removal must remain an independent "
               "ripple edit in the same document");
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
  const auto emptySelectionSnapshot = playback_video_edit::buildSnapshot(
      document, selection, true);
  ok &= expect(!selection.range() &&
                   !emptySelectionSnapshot.canTrim &&
                   !emptySelectionSnapshot.canRippleDelete,
               "an unmarked timeline must not advertise an edit operation");

  Selection finishSelection;
  finishSelection.markIn(document.timeline(), 1'000'000);
  finishSelection.markOut(document.timeline(), 1'966'667, 2'000'000);
  const auto finishSelectionSnapshot = playback_video_edit::buildSnapshot(
      document, finishSelection, true);
  ok &= expect(!finishSelectionSnapshot.hasUnexportedChanges,
               "an unapplied In/Out range must remain outside the persistent "
               "edit document");

  Selection inOnlySelection;
  inOnlySelection.markIn(document.timeline(), 2'000'000);
  const auto inOnlySnapshot = playback_video_edit::buildSnapshot(
      document, inOnlySelection, true);
  ok &= expect(!inOnlySelection.range() &&
                   !inOnlySnapshot.canTrim &&
                   !inOnlySnapshot.canRippleDelete,
               "a selection start alone must not imply a hidden end or an "
               "edit operation");

  Selection outOnlySelection;
  outOnlySelection.markOut(document.timeline(), 7'966'667, 8'000'000);
  const auto outOnlySnapshot = playback_video_edit::buildSnapshot(
      document, outOnlySelection, true);
  ok &= expect(!outOnlySelection.range() &&
                   !outOnlySnapshot.canTrim &&
                   !outOnlySnapshot.canRippleDelete &&
                   outOnlySnapshot.outFrameTimelineUs ==
                       std::optional<int64_t>(7'966'667),
               "a selection end alone must not imply a hidden start while "
               "retaining its exact inclusive frame position");

  Document oneSidedDocument;
  oneSidedDocument.load(10'000'000);
  ok &= expect(oneSidedDocument.trimTo({2'000'000, 10'000'000}) &&
                   oneSidedDocument.timeline().keptRanges() ==
                       std::vector<SourceRange>{{2'000'000, 10'000'000}},
               "the document must support trimming only its leading edge");
  ok &= expect(oneSidedDocument.discardAllChanges() &&
                   oneSidedDocument.trimTo({0, 8'000'000}) &&
                   oneSidedDocument.timeline().keptRanges() ==
                       std::vector<SourceRange>{{0, 8'000'000}},
               "the document must support trimming only its trailing edge");

  Selection completeSelection;
  completeSelection.markIn(document.timeline(), 0);
  completeSelection.markOut(document.timeline(), 9'966'667, 10'000'000);
  const auto completeSnapshot = playback_video_edit::buildSnapshot(
      document, completeSelection, true);
  ok &= expect(!completeSnapshot.canTrim &&
                   !completeSnapshot.canRippleDelete,
               "the complete sequence must be neither a trim change nor a "
               "valid emptying ripple delete");

  Selection clearableSelection;
  clearableSelection.markIn(document.timeline(), 1'000'000);
  clearableSelection.markOut(document.timeline(), 1'966'667, 2'000'000);
  ok &= expect(
      clearableSelection.hasMarks() &&
          clearableSelection.clear(playback_video_edit::EditBoundary::In) &&
          !clearableSelection.inSourceUs() &&
          clearableSelection.outSourceUs() ==
              std::optional<int64_t>(2'000'000) &&
          clearableSelection.outFrameSourceUs() ==
              std::optional<int64_t>(1'966'667),
      "an active In control must clear only its own mark");
  ok &= expect(clearableSelection.clear() &&
                   !clearableSelection.inSourceUs() &&
                   !clearableSelection.outSourceUs() &&
                   !clearableSelection.outFrameSourceUs() &&
                   !clearableSelection.hasMarks() &&
                   !clearableSelection.clear(),
               "Escape-style selection cancellation must clear all marks once");

  Selection crossedStart;
  crossedStart.markOut(document.timeline(), 4'966'667, 5'000'000,
                       100'000);
  crossedStart.markIn(document.timeline(), 7'000'000, 100'000);
  const auto crossedStartSnapshot = playback_video_edit::buildSnapshot(
      document, crossedStart, true);
  ok &= expect(crossedStartSnapshot.inTimelineUs ==
                       std::optional<int64_t>(4'900'000) &&
                   crossedStartSnapshot.outTimelineUs ==
                       std::optional<int64_t>(5'000'000),
               "setting Start beyond End must clamp Start without clearing "
               "or swapping End");

  Selection crossedEnd;
  crossedEnd.markIn(document.timeline(), 5'000'000, 100'000);
  crossedEnd.markOut(document.timeline(), 3'000'000, 3'100'000,
                     100'000);
  const auto crossedEndSnapshot = playback_video_edit::buildSnapshot(
      document, crossedEnd, true);
  ok &= expect(crossedEndSnapshot.inTimelineUs ==
                       std::optional<int64_t>(5'000'000) &&
                   crossedEndSnapshot.outTimelineUs ==
                       std::optional<int64_t>(5'100'000),
               "setting End before Start must clamp End without clearing or "
               "swapping Start");
  selection.markIn(document.timeline(), 3'000'000);
  selection.markOut(document.timeline(), 4'966'667, 5'000'000);
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
  const DecisionList exportedRevision = exported.timeline().decisionList();
  exported.markExported(exportedRevision);
  const auto exportedSnapshot = playback_video_edit::buildSnapshot(
      exported, Selection{}, true);
  ok &= expect(exportedSnapshot.hasEdits &&
                   !exportedSnapshot.hasUnexportedChanges,
               "a successful export must mark its exact document revision clean");
  Document exportedDiscard;
  exportedDiscard.load(10'000'000);
  ok &= expect(exportedDiscard.rippleDelete({2'000'000, 3'000'000}),
               "exported discard setup must create an edit decision");
  exportedDiscard.markExported(exportedDiscard.timeline().decisionList());
  ok &= expect(!exportedDiscard.hasUnexportedChanges() &&
                   exportedDiscard.discardAllChanges() &&
                   exportedDiscard.timeline().isUnmodified(),
               "discard must remain available after the current edit revision "
               "has been exported");
  ok &= expect(exported.rippleDelete({6'000'000, 7'000'000}) &&
                   exported.hasUnexportedChanges(),
               "editing after export must create a new dirty revision");
  ok &= expect(exported.undo() &&
                    exported.timeline().decisionList() == exportedRevision &&
                   !exported.hasUnexportedChanges(),
               "undoing to the exported revision must restore clean state");
  ok &= expect(exported.redo() && exported.hasUnexportedChanges(),
               "redoing past the exported revision must restore dirty state");

  const DecisionList secondExportedRevision =
      exported.timeline().decisionList();
  exported.markExported(secondExportedRevision);
  ok &= expect(!exported.hasUnexportedChanges() && exported.undo() &&
                   !exported.hasUnexportedChanges() && exported.redo() &&
                   !exported.hasUnexportedChanges(),
               "exporting a newer revision must not revoke the durable state "
               "of an older exported revision in undo history");

  Document asynchronousExport;
  asynchronousExport.load(10'000'000);
  ok &= expect(asynchronousExport.rippleDelete({2'000'000, 3'000'000}),
               "asynchronous export setup must create its first revision");
  const DecisionList queuedExportRevision =
      asynchronousExport.timeline().decisionList();
  ok &= expect(asynchronousExport.rippleDelete({6'000'000, 7'000'000}),
               "editing may continue while an older revision exports");
  asynchronousExport.markExported(queuedExportRevision);
  ok &= expect(asynchronousExport.hasUnexportedChanges(),
               "finishing an older export must not mark newer decisions clean");
  ok &= expect(asynchronousExport.undo() &&
                   !asynchronousExport.hasUnexportedChanges(),
               "the exported asynchronous revision must remain the clean baseline");

  Document transitionDocument;
  transitionDocument.load(10'000'000);
  ok &= expect(
      transitionDocument.rippleDelete({3'000'000, 5'000'000}) &&
          transitionDocument.setCutTransition(
              0, playback_video_edit::CutTransition::motionSmooth()),
      "a smooth cut must commit as an undoable edit-point decision");
  const DecisionList exportedTransitionRevision =
      transitionDocument.timeline().decisionList();
  transitionDocument.markExported(exportedTransitionRevision);
  const auto selectedSmoothCutSnapshot = playback_video_edit::buildSnapshot(
      transitionDocument, Selection{}, true, 3'000'000, 33'333);
  ok &= expect(!transitionDocument.hasUnexportedChanges() &&
                   selectedSmoothCutSnapshot.canToggleSmoothCut &&
                   selectedSmoothCutSnapshot.selectedCutTransition ==
                       std::optional<playback_video_edit::CutTransition>(
                           playback_video_edit::CutTransition::motionSmooth()),
               "the playhead must select the transition owned by its cut");
  ok &= expect(
      transitionDocument.setCutTransition(
          0, playback_video_edit::CutTransition::hard()) &&
          transitionDocument.hasUnexportedChanges() &&
          transitionDocument.undo() &&
          transitionDocument.timeline().decisionList() ==
              exportedTransitionRevision &&
          !transitionDocument.hasUnexportedChanges(),
      "transition changes must participate in dirty tracking and undo");

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
                       std::optional<int64_t>(2'000'000) &&
                   playback_video_edit::buildSnapshot(
                       draggedDocument, draggedOut, true)
                           .outFrameTimelineUs ==
                       std::optional<int64_t>(1'999'999),
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

  playback_video_edit::EditSnapshot hiddenOverlay;
  playback_video_edit::ExportProgress idleOverlayExport;
  playback_video_edit::EditSnapshot activeOverlay = hiddenOverlay;
  activeOverlay.active = true;
  playback_video_edit::EditSnapshot runningAnalysisOverlay = hiddenOverlay;
  runningAnalysisOverlay.sceneAnalysisStatus =
      playback_video_edit::SceneAnalysisStatus::Running;
  playback_video_edit::EditSnapshot failedAnalysisOverlay = hiddenOverlay;
  failedAnalysisOverlay.sceneAnalysisStatus =
      playback_video_edit::SceneAnalysisStatus::Failed;
  playback_video_edit::EditSnapshot readyAnalysisOverlay = hiddenOverlay;
  readyAnalysisOverlay.sceneAnalysisStatus =
      playback_video_edit::SceneAnalysisStatus::Ready;
  playback_video_edit::ExportProgress failedOverlayExport;
  failedOverlayExport.status = playback_video_edit::ExportStatus::Failed;
  ok &= expect(
      !playback_video_edit::needsOverlayPresentation(
          hiddenOverlay, idleOverlayExport, Prompt::None) &&
          playback_video_edit::needsOverlayPresentation(
              activeOverlay, idleOverlayExport, Prompt::None) &&
          playback_video_edit::needsOverlayPresentation(
              hiddenOverlay, idleOverlayExport, Prompt::LeavePlayback) &&
          playback_video_edit::needsOverlayPresentation(
              hiddenOverlay, failedOverlayExport, Prompt::None) &&
          playback_video_edit::needsOverlayPresentation(
              runningAnalysisOverlay, idleOverlayExport, Prompt::None) &&
          playback_video_edit::needsOverlayPresentation(
              failedAnalysisOverlay, idleOverlayExport, Prompt::None) &&
          !playback_video_edit::needsOverlayPresentation(
              readyAnalysisOverlay, idleOverlayExport, Prompt::None),
      "one editor presentation policy must own active, modal, failed, and "
      "background-analysis visibility");

  playback_video_edit::EditSnapshot overlayEdit;
  overlayEdit.active = true;
  overlayEdit.hasUnexportedChanges = true;
  overlayEdit.sourceDurationUs = 10'000'000;
  overlayEdit.timelineDurationUs = 8'000'000;
  overlayEdit.timecodeFrameDurationUs = 33'333;
  overlayEdit.keptRanges = {{0, 2'000'000}, {4'000'000, 10'000'000}};
  overlayEdit.clips = {{{0, 2'000'000}, 0},
                       {{4'000'000, 10'000'000}, 2'000'000}};
  overlayEdit.cuts = {
      {2'000'000, playback_video_edit::CutTransition::hard()}};
  overlayEdit.inTimelineUs = 2'000'000;
  overlayEdit.outTimelineUs = 4'000'000;
  overlayEdit.outFrameTimelineUs = 3'966'667;
  overlayEdit.playheadTimelineUs = 4'000'000;
  playback_video_edit::ExportProgress overlayExport;
  overlayExport.status = playback_video_edit::ExportStatus::Running;
  overlayExport.fraction = 0.42;
  overlayExport.targetsCurrentRevision = true;
  const playback_video_edit::OverlayModel overlayModel =
      playback_video_edit::buildOverlayModel(overlayEdit, &overlayExport,
                                              Prompt::None, 10, 0.5);
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
                    overlayModel.cutCells == std::vector<int>{2} &&
                    overlayModel.smoothCutCells.empty(),
                "marks, cuts, and playhead must share the program-time axis");
  playback_video_edit::EditSnapshot suggestedOverlay = overlayEdit;
  playback_video_edit::SceneSuggestionSnapshot suggestedScene;
  suggestedScene.id = 7;
  suggestedScene.source = {4'000'000, 6'000'000};
  suggestedScene.kind = playback_video_edit::SceneSuggestionKind::Cutscene;
  suggestedScene.confidence = 0.87f;
  suggestedScene.selected = true;
  suggestedScene.spans.push_back({2'000'000, 4'000'000});
  suggestedOverlay.sceneAnalysisStatus =
      playback_video_edit::SceneAnalysisStatus::Ready;
  suggestedOverlay.suggestionReview.visible = true;
  suggestedOverlay.suggestionReview.filteredCount = 1;
  suggestedOverlay.suggestionReview.selectedOrdinal = 1;
  suggestedOverlay.suggestionReview.suggestions.push_back(suggestedScene);
  suggestedOverlay.suggestionReview.selectedId = suggestedScene.id;
  const playback_video_edit::OverlayModel suggestedOverlayModel =
      playback_video_edit::buildOverlayModel(
          suggestedOverlay, nullptr, Prompt::None, 10, 0.5);
  const playback_video_edit::OverlayModel wideSuggestedOverlayModel =
      playback_video_edit::buildOverlayModel(
          suggestedOverlay, nullptr, Prompt::None, 80, 0.5);
  ok &= expect(suggestedOverlayModel.sceneSuggestionCells.size() == 10 &&
                   suggestedOverlayModel.sceneSuggestionCells[2] ==
                       playback_video_edit::SceneSuggestionCellKind::Selected &&
                   suggestedOverlayModel.sceneSuggestionBoundaryCells ==
                       std::vector<int>{2} &&
                   wideSuggestedOverlayModel.status.find("Strong") !=
                       std::string::npos &&
                   wideSuggestedOverlayModel.status.find("87%") ==
                       std::string::npos,
               "scene suggestions must share the edited timeline projection "
               "without becoming cut markers or exposing false-precision "
               "confidence");
  playback_video_edit::EditSnapshot analysingOverlay = overlayEdit;
  analysingOverlay.sceneAnalysisStatus =
      playback_video_edit::SceneAnalysisStatus::Running;
  analysingOverlay.sceneAnalysisProgress = 0.42;
  const playback_video_edit::OverlayModel analysingOverlayModel =
      playback_video_edit::buildOverlayModel(
          analysingOverlay, nullptr, Prompt::None, 40, 0.5);
  ok &= expect(analysingOverlayModel.status.find("SEGMENTS 42%") !=
                   std::string::npos,
               "background segment-detection progress must remain visible in "
               "the shared overlay model");
  const playback_video_edit::OverlayModel narrowAnalysingOverlayModel =
      playback_video_edit::buildOverlayModel(
          analysingOverlay, nullptr, Prompt::None, 8, 0.5);
  ok &= expect(narrowAnalysingOverlayModel.status.find("AI") ==
                   std::string::npos,
               "narrow status text must not leak an unexplained AI label");
  playback_video_edit::EditSnapshot smoothOverlay = overlayEdit;
  smoothOverlay.cuts.front().transition =
      playback_video_edit::CutTransition::motionSmooth();
  const playback_video_edit::OverlayModel smoothOverlayModel =
      playback_video_edit::buildOverlayModel(
          smoothOverlay, nullptr, Prompt::None, 10, 0.5);
  ok &= expect(smoothOverlayModel.cutCells.empty() &&
                   smoothOverlayModel.smoothCutCells == std::vector<int>{2},
               "a smooth cut must remain distinct in the shared timeline "
               "projection");
  ok &= expect(overlayModel.status == "EDITING*" &&
                    overlayModel.status.size() <= 10,
               "narrow editor status must keep the active mode visible");
  playback_video_edit::EditSnapshot inOnlyOverlay = overlayEdit;
  inOnlyOverlay.outTimelineUs.reset();
  const playback_video_edit::OverlayModel inOnlyOverlayModel =
      playback_video_edit::buildOverlayModel(
          inOnlyOverlay, nullptr, Prompt::None, 10, 0.5);
  ok &= expect(inOnlyOverlayModel.cells[1] ==
                       playback_video_edit::TimelineCellKind::Kept &&
                   inOnlyOverlayModel.cells[2] ==
                       playback_video_edit::TimelineCellKind::KeptAlternate &&
                   inOnlyOverlayModel.cells[9] ==
                       playback_video_edit::TimelineCellKind::KeptAlternate &&
                   inOnlyOverlayModel.inCell == std::optional<int>(2) &&
                   !inOnlyOverlayModel.outCell,
               "a lone Start handle must not imply or paint a hidden End");
  playback_video_edit::EditSnapshot outOnlyOverlay = overlayEdit;
  outOnlyOverlay.inTimelineUs.reset();
  const playback_video_edit::OverlayModel outOnlyOverlayModel =
      playback_video_edit::buildOverlayModel(
          outOnlyOverlay, nullptr, Prompt::None, 10, 0.5);
  ok &= expect(outOnlyOverlayModel.cells[0] ==
                       playback_video_edit::TimelineCellKind::Kept &&
                   outOnlyOverlayModel.cells[4] ==
                       playback_video_edit::TimelineCellKind::KeptAlternate &&
                   outOnlyOverlayModel.cells[5] ==
                       playback_video_edit::TimelineCellKind::KeptAlternate &&
                   !outOnlyOverlayModel.inCell &&
                   outOnlyOverlayModel.outCell == std::optional<int>(5),
               "a lone End handle must not imply or paint a hidden Start");
  const playback_video_edit::OverlayModel tinyExportModel =
      playback_video_edit::buildOverlayModel(overlayEdit, &overlayExport,
                                              Prompt::None, 6, 0.5);
  ok &= expect(tinyExportModel.status == "EDIT*",
               "tiny editor status must retain a compact mode indicator");
  const playback_video_edit::OverlayModel tinyDirtyModel =
      playback_video_edit::buildOverlayModel(
          overlayEdit, nullptr, Prompt::None, 1, 0.5);
  ok &= expect(tinyDirtyModel.status == "*",
               "one-column editor status must retain the dirty indicator");
  const playback_video_edit::OverlayModel wideOverlayModel =
      playback_video_edit::buildOverlayModel(overlayEdit, &overlayExport,
                                              Prompt::None, 96, 0.5);
  ok &= expect(wideOverlayModel.status.find("EXPORT 42%") !=
                        std::string::npos &&
                    wideOverlayModel.status.find("TC 00:00:04:00") !=
                        std::string::npos &&
                    wideOverlayModel.status.find("SELECTED 00:02:00") !=
                        std::string::npos &&
                    wideOverlayModel.status.find("1 REMOVED") !=
                        std::string::npos &&
                    wideOverlayModel.status.find("EDITING*") !=
                        std::string::npos &&
                    wideOverlayModel.status.find("Ctrl+") ==
                        std::string::npos &&
                    wideOverlayModel.status.size() <= 96,
               "wide editor status must identify the selection and prior "
               "removals without duplicating controls");
  const playback_video_edit::OverlayModel rangeOverlayModel =
      playback_video_edit::buildOverlayModel(overlayEdit, nullptr,
                                              Prompt::None, 34, 0.5);
  ok &= expect(rangeOverlayModel.status ==
                   "EDITING*  SELECTED 00:02:00",
               "compact editor status must describe the selected section "
               "without exposing In/Out jargon");
  playback_video_edit::EditSnapshot vfrOutOverlay = overlayEdit;
  vfrOutOverlay.outFrameTimelineUs = 3'950'000;
  const playback_video_edit::OverlayModel vfrOutOverlayModel =
      playback_video_edit::buildOverlayModel(
          vfrOutOverlay, nullptr, Prompt::None, 64, 0.5);
  ok &= expect(vfrOutOverlayModel.status.find("SELECTED 00:02:00") !=
                   std::string::npos,
               "selection duration must remain stable when the exact VFR end "
               "frame PTS differs from nominal cadence");
  const playback_video_edit::OverlayModel inOnlyDurationModel =
      playback_video_edit::buildOverlayModel(
          inOnlyOverlay, nullptr, Prompt::None, 64, 0.5);
  ok &= expect(inOnlyDurationModel.status.find("START 00:02:00") !=
                       std::string::npos &&
                   inOnlyDurationModel.status.find("SELECTED") ==
                       std::string::npos,
               "a lone Start must remain an incomplete selection");
  const playback_video_edit::OverlayModel outOnlyDurationModel =
      playback_video_edit::buildOverlayModel(
          outOnlyOverlay, nullptr, Prompt::None, 64, 0.5);
  ok &= expect(outOnlyDurationModel.status.find("END 00:03:29") !=
                       std::string::npos &&
                   outOnlyDurationModel.status.find("SELECTED") ==
                       std::string::npos,
               "a lone End must remain an incomplete selection");

  playback_video_edit::ExportProgress failedExport;
  failedExport.status = playback_video_edit::ExportStatus::Failed;
  const playback_video_edit::OverlayModel failedExportModel =
      playback_video_edit::buildOverlayModel(
          playback_video_edit::EditSnapshot{}, &failedExport, Prompt::None,
          20, 0.0);
  const playback_video_edit::OverlayModel tinyFailedExportModel =
      playback_video_edit::buildOverlayModel(
          playback_video_edit::EditSnapshot{}, &failedExport, Prompt::None,
          6, 0.0);
  const playback_video_edit::OverlayModel failedExitModel =
      playback_video_edit::buildOverlayModel(
          playback_video_edit::EditSnapshot{}, &failedExport,
          Prompt::LeavePlayback, 20, 0.0);
  playback_video_edit::EditSnapshot activeFailedEdit = overlayEdit;
  const playback_video_edit::OverlayModel activeFailedExportModel =
      playback_video_edit::buildOverlayModel(
          activeFailedEdit, &failedExport, Prompt::None, 10, 0.0);
  ok &= expect(failedExport.visible() && !failedExport.running() &&
                   failedExport.failed() &&
                   failedExportModel.status == "EXPORT FAILED" &&
                   failedExportModel.cells.empty() &&
                   tinyFailedExportModel.status == "FAILED" &&
                   failedExitModel.status == "EXPORT FAILED" &&
                   activeFailedExportModel.status == "FAILED" &&
                   !activeFailedExportModel.cells.empty(),
               "a failed current-revision export must remain compactly "
               "visible in and outside edit mode, including while playback "
               "exit is pending");

  playback_video_edit::EditSnapshot retainedProgram;
  retainedProgram.hasEdits = true;
  retainedProgram.hasUnexportedChanges = true;
  ok &= expect(playback_video_edit::retainedProgramBadge(retainedProgram) ==
                   "[EDITED*]",
               "shared chrome must identify an unexported program timeline "
               "after edit mode closes");
  retainedProgram.hasUnexportedChanges = false;
  ok &= expect(playback_video_edit::retainedProgramBadge(retainedProgram) ==
                   "[EDITED]",
               "an exported edited program must remain identifiable without "
               "claiming that it is dirty");
  retainedProgram.active = true;
  ok &= expect(playback_video_edit::retainedProgramBadge(retainedProgram)
                   .empty(),
               "the edit-mode timeline status must not duplicate the edited "
               "program marker in the title");

  playback_overlay::PlaybackOverlayState editorControlState;
  editorControlState.videoEdit.active = true;
  editorControlState.playPauseAvailable = true;
  editorControlState.paused = true;
  playback_overlay::PlaybackOverlayState playbackSuffixState;
  playbackSuffixState.displaySec = 4.0;
  playbackSuffixState.totalSec = 8.0;
  playbackSuffixState.volPct = 100;
  ok &= expect(playback_overlay::buildWindowOverlayProgressSuffix(
                   playbackSuffixState) == "00:04 / 00:08",
               "playback chrome must not claim a volume when audio is "
               "unavailable");
  playbackSuffixState.audioOk = true;
  ok &= expect(playback_overlay::buildWindowOverlayProgressSuffix(
                   playbackSuffixState) == "00:04 / 00:08 Vol: 100%",
               "playback chrome must retain volume for an active audio "
               "output");
  playbackSuffixState.videoEdit.active = true;
  ok &= expect(playback_overlay::buildWindowOverlayProgressSuffix(
                   playbackSuffixState).empty(),
               "edit mode must own its frame-accurate timeline row without "
               "a duplicate rounded playback clock");
  playbackSuffixState.videoEdit.active = false;
  playbackSuffixState.videoEditPrompt = Prompt::LeavePlayback;
  ok &= expect(playback_overlay::buildWindowOverlayProgressSuffix(
                   playbackSuffixState).empty(),
               "a modal editor prompt must own its chrome without an "
               "unrelated playback suffix");
  const auto emptyEditControls =
      playback_overlay::buildOverlayControlSpecs(editorControlState, -1);
  const std::vector<playback_overlay::OverlayControlId> expectedEmptyControls{
      playback_overlay::OverlayControlId::EditMarkIn,
      playback_overlay::OverlayControlId::EditMarkOut,
      playback_overlay::OverlayControlId::EditSuggestions,
      playback_overlay::OverlayControlId::PlayPause,
      playback_overlay::OverlayControlId::EditDone,
  };
  const auto controlIds = [](const auto& specs) {
    std::vector<playback_overlay::OverlayControlId> ids;
    ids.reserve(specs.size());
    for (const auto& spec : specs) ids.push_back(spec.id);
    return ids;
  };
  const auto controlFor = [](const auto& specs,
                             playback_overlay::OverlayControlId id) {
    return std::find_if(specs.begin(), specs.end(),
                        [&](const auto& spec) { return spec.id == id; });
  };
  const auto startControl = controlFor(
      emptyEditControls, playback_overlay::OverlayControlId::EditMarkIn);
  const auto endControl = controlFor(
      emptyEditControls, playback_overlay::OverlayControlId::EditMarkOut);
  ok &= expect(controlIds(emptyEditControls) == expectedEmptyControls &&
                   startControl != emptyEditControls.end() &&
                   startControl->normalText == " [Start] " &&
                   endControl != emptyEditControls.end() &&
                   endControl->normalText == " [End] ",
               "an empty range tool must offer plain-language Start and End "
               "before transport or document actions");

  editorControlState.videoEdit.inTimelineUs = 1'000'000;
  const auto partialEditControls =
      playback_overlay::buildOverlayControlSpecs(editorControlState, -1);
  const std::vector<playback_overlay::OverlayControlId>
      expectedPartialControls{
          playback_overlay::OverlayControlId::EditMarkIn,
          playback_overlay::OverlayControlId::EditMarkOut,
          playback_overlay::OverlayControlId::EditClearSelection,
          playback_overlay::OverlayControlId::EditSuggestions,
          playback_overlay::OverlayControlId::PlayPause,
          playback_overlay::OverlayControlId::EditDone,
      };
  const auto activeStart = controlFor(
      partialEditControls, playback_overlay::OverlayControlId::EditMarkIn);
  ok &= expect(controlIds(partialEditControls) == expectedPartialControls &&
                   activeStart != partialEditControls.end() &&
                   activeStart->active &&
                   std::none_of(
                       partialEditControls.begin(), partialEditControls.end(),
                       [](const auto& control) {
                         return control.id == playback_overlay::OverlayControlId::
                                                  EditRippleDelete ||
                                control.id == playback_overlay::OverlayControlId::
                                                  EditTrim;
                       }),
               "one endpoint must remain an incomplete, cancellable "
               "selection instead of implying a hidden trim range");

  editorControlState.videoEdit.outTimelineUs = 2'000'000;
  editorControlState.videoEdit.canRippleDelete = true;
  editorControlState.videoEdit.canTrim = true;
  editorControlState.videoEdit.canUndo = true;
  editorControlState.videoEdit.canRedo = true;
  editorControlState.videoEdit.hasEdits = true;
  editorControlState.videoEdit.hasUnexportedChanges = false;
  const auto availableEditControls =
      playback_overlay::buildOverlayControlSpecs(editorControlState, -1);
  const std::vector<playback_overlay::OverlayControlId>
      expectedCompleteControls{
          playback_overlay::OverlayControlId::EditRippleDelete,
          playback_overlay::OverlayControlId::EditTrim,
          playback_overlay::OverlayControlId::EditClearSelection,
          playback_overlay::OverlayControlId::EditSuggestions,
          playback_overlay::OverlayControlId::PlayPause,
          playback_overlay::OverlayControlId::EditDone,
      };
  ok &= expect(controlIds(availableEditControls) == expectedCompleteControls,
               "a complete range must replace endpoint tools with its two "
               "explicit operations and a local Cancel action");
  for (const auto id : {
           playback_overlay::OverlayControlId::EditRippleDelete,
           playback_overlay::OverlayControlId::EditTrim,
       }) {
    const auto control = controlFor(availableEditControls, id);
    ok &= expect(control != availableEditControls.end() && control->enabled,
                 "available edit commands must enable in their existing slots");
  }
  const auto pausedControl = controlFor(
      availableEditControls, playback_overlay::OverlayControlId::PlayPause);
  const int pausedControlWidth =
      pausedControl != availableEditControls.end() ? pausedControl->width : -1;
  editorControlState.paused = false;
  const auto playingEditControls =
      playback_overlay::buildOverlayControlSpecs(editorControlState, -1);
  const auto playingControl = controlFor(
      playingEditControls, playback_overlay::OverlayControlId::PlayPause);
  ok &= expect(playingControl != playingEditControls.end() &&
                   playingControl->width == pausedControlWidth,
               "Play and Pause labels must reserve one stable toolbar slot");
  editorControlState.videoEditExport.status =
      playback_video_edit::ExportStatus::Running;
  const auto exportingEditControls =
      playback_overlay::buildOverlayControlSpecs(editorControlState, -1);
  ok &= expect(controlIds(exportingEditControls) ==
                   expectedCompleteControls,
               "background export state must not repurpose the monitor bar");
  std::optional<playback_video_edit::Command> dispatchedEditCommand;
  playback_overlay::OverlayControlActions editControlActions;
  editControlActions.videoEdit = [&](playback_video_edit::Command command) {
    dispatchedEditCommand = command;
    return true;
  };
  ok &= expect(playback_overlay::dispatchOverlayControl(
                   playback_overlay::OverlayControlId::EditDone,
                   editControlActions) &&
                   dispatchedEditCommand ==
                       playback_video_edit::Command::Finish,
               "Done must only leave the edit tools instead of entering the "
               "Escape confirmation path or implying an export");
  ok &= expect(playback_overlay::dispatchOverlayControl(
                   playback_overlay::OverlayControlId::EditMarkIn,
                   editControlActions) &&
                   dispatchedEditCommand ==
                       playback_video_edit::Command::ToggleIn &&
                   playback_overlay::dispatchOverlayControl(
                       playback_overlay::OverlayControlId::EditMarkOut,
                       editControlActions) &&
                   dispatchedEditCommand ==
                       playback_video_edit::Command::ToggleOut &&
                   playback_overlay::dispatchOverlayControl(
                       playback_overlay::OverlayControlId::EditClearSelection,
                       editControlActions) &&
                   dispatchedEditCommand ==
                       playback_video_edit::Command::ClearInAndOut,
               "endpoint buttons must toggle and Cancel must clear only the "
               "local range-selection tool");

  playback_overlay::PlaybackOverlayState suggestionControlState;
  suggestionControlState.videoEdit.active = true;
  suggestionControlState.playPauseAvailable = true;
  suggestionControlState.paused = true;
  suggestionControlState.videoEdit.sceneAnalysisStatus =
      playback_video_edit::SceneAnalysisStatus::Ready;
  suggestionControlState.videoEdit.suggestionReview.visible = true;
  suggestionControlState.videoEdit.suggestionReview.filter =
      playback_video_edit::SceneSuggestionFilter::Cutscenes;
  suggestionControlState.videoEdit.suggestionReview.totalCount = 8;
  suggestionControlState.videoEdit.suggestionReview.filteredCount = 3;
  suggestionControlState.videoEdit.suggestionReview.selectedId = 42;
  suggestionControlState.videoEdit.suggestionReview.canUndoHide = true;
  const auto suggestionControls = playback_overlay::buildOverlayControlSpecs(
      suggestionControlState, -1);
  const std::vector<playback_overlay::OverlayControlId>
      expectedSuggestionControls{
          playback_overlay::OverlayControlId::EditMarkIn,
          playback_overlay::OverlayControlId::EditMarkOut,
          playback_overlay::OverlayControlId::EditSuggestions,
          playback_overlay::OverlayControlId::EditSuggestionFilter,
          playback_overlay::OverlayControlId::EditPreviousSuggestion,
          playback_overlay::OverlayControlId::EditNextSuggestion,
          playback_overlay::OverlayControlId::EditSelectSuggestion,
          playback_overlay::OverlayControlId::EditHideSuggestion,
          playback_overlay::OverlayControlId::EditUndoHideSuggestion,
          playback_overlay::OverlayControlId::PlayPause,
          playback_overlay::OverlayControlId::EditDone,
      };
  const auto suggestionsControl = controlFor(
      suggestionControls,
      playback_overlay::OverlayControlId::EditSuggestions);
  const auto suggestionFilterControl = controlFor(
      suggestionControls,
      playback_overlay::OverlayControlId::EditSuggestionFilter);
  ok &= expect(
      controlIds(suggestionControls) == expectedSuggestionControls &&
          suggestionsControl != suggestionControls.end() &&
          suggestionsControl->normalText == " [Suggestions 8] " &&
          suggestionsControl->active &&
          suggestionFilterControl != suggestionControls.end() &&
          suggestionFilterControl->normalText == " [Filter: Cutscenes] ",
      "the shared editor toolbar must expose a persistent, filterable "
      "suggestion review workflow");
  for (const auto [control, command] : {
           std::pair{playback_overlay::OverlayControlId::EditSuggestions,
                     playback_video_edit::Command::ToggleSceneSuggestions},
           std::pair{
               playback_overlay::OverlayControlId::EditSuggestionFilter,
               playback_video_edit::Command::CycleSceneSuggestionFilter},
           std::pair{
               playback_overlay::OverlayControlId::EditPreviousSuggestion,
               playback_video_edit::Command::PreviousSceneSuggestion},
           std::pair{playback_overlay::OverlayControlId::EditNextSuggestion,
                     playback_video_edit::Command::NextSceneSuggestion},
           std::pair{
               playback_overlay::OverlayControlId::EditSelectSuggestion,
               playback_video_edit::Command::SelectSceneSuggestion},
           std::pair{playback_overlay::OverlayControlId::EditHideSuggestion,
                     playback_video_edit::Command::DismissSceneSuggestion},
           std::pair{
               playback_overlay::OverlayControlId::EditUndoHideSuggestion,
               playback_video_edit::Command::UndoDismissSceneSuggestion},
       }) {
    dispatchedEditCommand.reset();
    ok &= expect(playback_overlay::dispatchOverlayControl(
                     control, editControlActions) &&
                     dispatchedEditCommand == command,
                 "each suggestion control must dispatch through the shared "
                 "editor command boundary");
  }
  ok &= expect(playback_overlay::dispatchOverlayControl(
                   playback_overlay::OverlayControlId::EditStartExport,
                   editControlActions) &&
                   dispatchedEditCommand ==
                       playback_video_edit::Command::StartExport &&
                   playback_overlay::dispatchOverlayControl(
                       playback_overlay::OverlayControlId::EditCancelExport,
                       editControlActions) &&
                   dispatchedEditCommand ==
                       playback_video_edit::Command::CancelExport,
               "rendered export and cancel controls must dispatch distinct "
               "commands rather than a worker-state toggle");
  bool waitedForExport = false;
  editControlActions.waitForVideoEditExport = [&]() {
    waitedForExport = true;
    return true;
  };
  dispatchedEditCommand.reset();
  ok &= expect(playback_overlay::dispatchOverlayControl(
                   playback_overlay::OverlayControlId::EditWaitForExport,
                   editControlActions) &&
                   waitedForExport && !dispatchedEditCommand,
               "waiting for an exit export must remain a session action, not "
               "an encoder command");

  editorControlState.pictureInPictureAvailable = true;
  editorControlState.pictureInPictureActive = true;
  const auto pictureInPictureEditControls =
      playback_overlay::buildOverlayControlSpecs(editorControlState, -1);
  const std::vector<playback_overlay::OverlayControlId>
      expectedPictureInPictureEditControls{
          playback_overlay::OverlayControlId::PictureInPicture,
          playback_overlay::OverlayControlId::EditRippleDelete,
          playback_overlay::OverlayControlId::EditTrim,
          playback_overlay::OverlayControlId::EditClearSelection,
          playback_overlay::OverlayControlId::EditSuggestions,
          playback_overlay::OverlayControlId::PlayPause,
          playback_overlay::OverlayControlId::EditDone,
      };
  ok &= expect(
      controlIds(pictureInPictureEditControls) ==
              expectedPictureInPictureEditControls &&
          pictureInPictureEditControls.front().normalText ==
              " [Close PiP] " &&
          std::none_of(
              pictureInPictureEditControls.begin(),
              pictureInPictureEditControls.end(), [](const auto& control) {
                return control.id == playback_overlay::OverlayControlId::Radio ||
                       control.id ==
                           playback_overlay::OverlayControlId::AudioTrack ||
                       control.id ==
                           playback_overlay::OverlayControlId::Subtitles;
              }),
      "PiP must project the same editor command model, with an immediately "
      "reachable close action and no playback-only toolbar");

  playback_overlay::PlaybackOverlayState pendingExitControlState;
  pendingExitControlState.videoEditPrompt = Prompt::LeavePlayback;
  pendingExitControlState.videoEdit.hasUnexportedChanges = true;
  pendingExitControlState.videoEditExport.status =
      playback_video_edit::ExportStatus::Running;
  pendingExitControlState.videoEditExport.targetsCurrentRevision = true;
  const auto pendingExitControls = playback_overlay::buildOverlayControlSpecs(
      pendingExitControlState, -1);
  ok &= expect(pendingExitControls.size() == 3 &&
                   pendingExitControls.front().id ==
                       playback_overlay::OverlayControlId::EditWaitForExport &&
                   pendingExitControls[1].id ==
                       playback_overlay::OverlayControlId::EditCancelExport &&
                   pendingExitControls.front().normalText == " [Wait] " &&
                   std::none_of(
                       pendingExitControls.begin(), pendingExitControls.end(),
                       [](const auto& control) {
                         return control.id == playback_overlay::OverlayControlId::
                                                  EditDiscardAndExit;
                       }),
               "a current-revision export must offer wait or explicit cancel "
               "without conflating either action with discard");

  pendingExitControlState.videoEditExport.targetsCurrentRevision = false;
  const auto blockingExportControls =
      playback_overlay::buildOverlayControlSpecs(pendingExitControlState, -1);
  ok &= expect(blockingExportControls.size() == 2 &&
                   blockingExportControls.front().id ==
                       playback_overlay::OverlayControlId::EditCancelExport &&
                   blockingExportControls.front().normalText ==
                       " [Cancel export] " &&
                   std::none_of(
                       blockingExportControls.begin(),
                       blockingExportControls.end(), [](const auto& control) {
                         return control.id == playback_overlay::OverlayControlId::
                                                  EditDiscardAndExit;
                       }),
               "an older export must be identified as blocking instead of "
               "pretending that waiting or discard resolves both revisions");

  pendingExitControlState.videoEditExport = {};
  const auto resolvedExportControls =
      playback_overlay::buildOverlayControlSpecs(pendingExitControlState, -1);
  ok &= expect(resolvedExportControls.size() == 3 &&
                   resolvedExportControls.front().id ==
                       playback_overlay::OverlayControlId::EditStartExport &&
                   resolvedExportControls[1].id ==
                       playback_overlay::OverlayControlId::EditDiscardAndExit,
               "export and discard may be offered only after no worker is "
               "still holding an output revision");

  const int disabledDeleteToken = playback_overlay::overlayControlToken(
      playback_overlay::OverlayControlId::EditRippleDelete);
  playback_overlay::PlaybackOverlayState disabledControlState;
  disabledControlState.playPauseAvailable = true;
  disabledControlState.videoEdit.active = true;
  disabledControlState.videoEdit.inTimelineUs = 1'000'000;
  disabledControlState.videoEdit.outTimelineUs = 2'000'000;
  const auto disabledHoverSpecs = playback_overlay::buildOverlayControlSpecs(
      disabledControlState, disabledDeleteToken);
  const auto disabledInputs = playback_overlay::buildOverlayCellControlInputs(
      disabledHoverSpecs, disabledDeleteToken);
  const auto disabledLayout =
      playback_overlay::layoutOverlayControlCells(disabledInputs, 160);
  const auto disabledMap =
      playback_overlay::buildOverlayInteractionMap(disabledLayout);
  const auto disabledDelete = std::find_if(
      disabledLayout.controls.begin(), disabledLayout.controls.end(),
      [](const auto& item) {
        return item.id ==
               playback_overlay::OverlayControlId::EditRippleDelete;
      });
  const auto enabledCancel = std::find_if(
      disabledLayout.controls.begin(), disabledLayout.controls.end(),
      [](const auto& item) {
        return item.id ==
               playback_overlay::OverlayControlId::EditClearSelection;
      });
  ok &= expect(disabledDelete != disabledLayout.controls.end() &&
                   !disabledDelete->enabled && !disabledDelete->hovered &&
                   !playback_overlay::overlayControlAt(
                       disabledMap, disabledDelete->x + 0.5,
                       disabledDelete->y + 0.5),
               "disabled controls must render without accepting hover or clicks");
  ok &= expect(enabledCancel != disabledLayout.controls.end() &&
                   playback_overlay::overlayControlAt(
                       disabledMap, enabledCancel->x + 0.5,
                       enabledCancel->y + 0.5) ==
                       playback_overlay::OverlayControlId::
                           EditClearSelection,
               "enabled controls must retain normal semantic hit-testing");

  playback_overlay::OverlayCellLayoutInput shortSurface;
  shortSurface.width = 24;
  shortSurface.height = 4;
  shortSurface.title = "example.mp4";
  shortSurface.suffix = "00:04 / 00:08";
  shortSurface.reservedRowsAboveProgress = 1;
  shortSurface.controls = {
      {playback_overlay::OverlayControlId::EditMarkIn, "[Start]", 7},
      {playback_overlay::OverlayControlId::EditMarkOut, "[End]", 5},
      {playback_overlay::OverlayControlId::PlayPause, "[Play]", 6},
      {playback_overlay::OverlayControlId::EditDone, "[Done]", 6},
  };
  const playback_overlay::OverlayCellLayout shortLayout =
      playback_overlay::layoutOverlayCells(shortSurface);
  ok &= expect(shortLayout.controls.size() == 3 &&
                   shortLayout.controls[0].id ==
                       playback_overlay::OverlayControlId::EditMarkIn &&
                   shortLayout.controls[1].id ==
                       playback_overlay::OverlayControlId::EditMarkOut &&
                   shortLayout.controls[2].id ==
                       playback_overlay::OverlayControlId::PlayPause &&
                   std::all_of(shortLayout.controls.begin(),
                               shortLayout.controls.end(),
                               [](const auto& control) {
                                 return control.y >= 0;
                               }),
               "a short ASCII or PiP surface must retain its leading primary "
               "control row instead of exposing later commands");
  shortSurface.height = 1;
  const playback_overlay::OverlayCellLayout oneRowLayout =
      playback_overlay::layoutOverlayCells(shortSurface);
  ok &= expect(oneRowLayout.controls.empty() && oneRowLayout.suffixY == -1 &&
                   oneRowLayout.topY == 0,
               "an undersized overlay must omit content that has no row "
               "instead of publishing negative geometry");
  overlayEdit.inTimelineUs.reset();
  overlayEdit.outTimelineUs.reset();
  const playback_video_edit::OverlayModel unselectedOverlayModel =
      playback_video_edit::buildOverlayModel(
          overlayEdit, nullptr, Prompt::None, 10, 0.0);
  ok &= expect(unselectedOverlayModel.cells[2] ==
                   playback_video_edit::TimelineCellKind::KeptAlternate &&
                   unselectedOverlayModel.cutCells == std::vector<int>{2},
               "removed sections must remain visible as distinct adjacent "
               "clips separated by explicit cut points");
  const playback_video_edit::OverlayModel unselectedWideOverlayModel =
      playback_video_edit::buildOverlayModel(
          overlayEdit, nullptr, Prompt::None, 64, 0.0);
  ok &= expect(unselectedWideOverlayModel.status.find("SELECTED") ==
                       std::string::npos &&
                   unselectedWideOverlayModel.status.find("1 REMOVED") !=
                       std::string::npos,
               "an unselected edited program must show accumulated removals "
               "without implying an active range");
  playback_video_edit::EditSnapshot multipleRemovalOverlay = overlayEdit;
  multipleRemovalOverlay.timelineDurationUs = 6'000'000;
  multipleRemovalOverlay.keptRanges = {
      {0, 2'000'000}, {4'000'000, 6'000'000}, {8'000'000, 10'000'000}};
  multipleRemovalOverlay.clips = {
      {{0, 2'000'000}, 0},
      {{4'000'000, 6'000'000}, 2'000'000},
      {{8'000'000, 10'000'000}, 4'000'000},
  };
  multipleRemovalOverlay.cuts = {
      {2'000'000, playback_video_edit::CutTransition::hard()},
      {4'000'000, playback_video_edit::CutTransition::hard()},
  };
  multipleRemovalOverlay.inTimelineUs.reset();
  multipleRemovalOverlay.outTimelineUs.reset();
  multipleRemovalOverlay.outFrameTimelineUs.reset();
  const playback_video_edit::OverlayModel multipleRemovalModel =
      playback_video_edit::buildOverlayModel(
          multipleRemovalOverlay, nullptr, Prompt::None, 12, 0.0);
  const playback_video_edit::OverlayModel multipleRemovalStatusModel =
      playback_video_edit::buildOverlayModel(
          multipleRemovalOverlay, nullptr, Prompt::None, 64, 0.0);
  ok &= expect(
      multipleRemovalModel.cells[0] ==
              playback_video_edit::TimelineCellKind::Kept &&
          multipleRemovalModel.cells[4] ==
              playback_video_edit::TimelineCellKind::KeptAlternate &&
          multipleRemovalModel.cells[8] ==
              playback_video_edit::TimelineCellKind::Kept &&
          multipleRemovalModel.cutCells == std::vector<int>({4, 7}) &&
          multipleRemovalStatusModel.status.find("2 REMOVED") !=
              std::string::npos,
      "multiple removals must stay visibly separate as alternating clips, "
      "cut markers, and an accumulated count");
  const playback_video_edit::OverlayModel multipleRemovalPromptBaseline =
      playback_video_edit::buildOverlayModel(
          multipleRemovalOverlay, nullptr, Prompt::None, 20, 0.0);
  const playback_video_edit::OverlayModel multipleRemovalPromptModel =
      playback_video_edit::buildOverlayModel(
          multipleRemovalOverlay, nullptr, Prompt::LeaveEditMode, 20, 0.0);
  ok &= expect(
      multipleRemovalPromptModel.status == "LEAVE EDIT MODE?" &&
          multipleRemovalPromptModel.cells ==
              multipleRemovalPromptBaseline.cells &&
          multipleRemovalPromptModel.playheadCell ==
              multipleRemovalPromptBaseline.playheadCell &&
          multipleRemovalPromptModel.cutCells ==
              multipleRemovalPromptBaseline.cutCells,
      "opening a modal editor prompt must preserve the current program "
      "timeline instead of revealing the playback progress bar");
  overlayEdit.active = false;
  playback_video_edit::ExportProgress olderOverlayExport = overlayExport;
  olderOverlayExport.targetsCurrentRevision = false;
  const playback_video_edit::OverlayModel backgroundExportModel =
      playback_video_edit::buildOverlayModel(overlayEdit, &olderOverlayExport,
                                              Prompt::None, 10, 0.0);
  ok &= expect(backgroundExportModel.cells.empty() &&
                    backgroundExportModel.status == "OLD EXPORT",
                "a background job for an older decision list must remain "
                "visible without claiming to export the current revision");

  const playback_video_edit::OverlayModel exitModel =
      playback_video_edit::buildOverlayModel(
          overlayEdit, nullptr, Prompt::LeavePlayback, 10, 0.0);
  ok &= expect(exitModel.status == "UNEXPORTED" &&
                   exitModel.status.size() <= 10,
               "exit confirmation must use a complete width-bounded state");
  playback_video_edit::ExportProgress blockingExitExport;
  blockingExitExport.status = playback_video_edit::ExportStatus::Running;
  blockingExitExport.fraction = 0.42;
  const playback_video_edit::OverlayModel blockingExitModel =
      playback_video_edit::buildOverlayModel(
          overlayEdit, &blockingExitExport, Prompt::LeavePlayback, 40, 0.0);
  ok &= expect(blockingExitModel.status ==
                   "UNEXPORTED EDITS  EXPORT BUSY",
               "playback exit must expose an older blocking job without "
               "claiming that it saves the current revision");
  overlayEdit.hasUnexportedChanges = false;
  const playback_video_edit::OverlayModel hazardFreeExitModel =
      playback_video_edit::buildOverlayModel(
          overlayEdit, nullptr, Prompt::LeavePlayback, 20, 0.0);
  ok &= expect(hazardFreeExitModel.status == "LEAVE PLAYBACK?",
               "a completion race with no remaining hazard must not invent "
               "unexported edits");
  overlayEdit.hasUnexportedChanges = true;

  const playback_video_edit::OverlayModel closeEditorModel =
      playback_video_edit::buildOverlayModel(
          overlayEdit, nullptr, Prompt::LeaveEditMode, 16, 0.0);
  ok &= expect(closeEditorModel.status == "LEAVE EDIT MODE?",
               "leaving only edit mode must have its own explicit prompt");

  const playback_video_edit::OverlayModel discardModel =
      playback_video_edit::buildOverlayModel(
          overlayEdit, nullptr, Prompt::DiscardEdits, 18, 0.0);
  ok &= expect(discardModel.status == "DISCARD ALL EDITS?",
               "discarding edit history must have its own explicit prompt");

  playback_overlay::OverlayCellLayout promptLayout;
  promptLayout.progressBarX = 2;
  promptLayout.progressBarY = 4;
  promptLayout.progressBarWidth = 20;
  const playback_overlay::InteractionMap discardInteractions =
      playback_overlay::buildOverlayInteractionMap(
          promptLayout, &overlayEdit, Prompt::DiscardEdits);
  ok &= expect(discardInteractions.modal &&
                   discardInteractions.contains(-100.0, -100.0) &&
                   !discardInteractions.progressBar,
               "a visible editor prompt must capture the entire input surface");
  const playback_overlay::InteractionMap ordinaryInteractions =
      playback_overlay::buildOverlayInteractionMap(
          promptLayout, &overlayEdit, Prompt::None);
  ok &= expect(!ordinaryInteractions.modal &&
                   ordinaryInteractions.progressBar.has_value(),
               "ordinary edit mode must restore precise timeline hit-testing");

  playback_overlay::InteractionMap interactions;
  interactions.progressBar =
      playback_overlay::ProgressBarRegion{{100.0, 200.0, 200.0, 210.0}, 10};
  interactions.controls.push_back(
      {{20.0, 30.0, 40.0, 40.0},
       playback_overlay::OverlayControlId::EditStartExport});
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
                   playback_overlay::OverlayControlId::EditStartExport,
               "rendered controls must retain their semantic identity");
  ok &= expect(playback_overlay::editBoundaryAt(interactions, 100.0, 205.0) ==
                   playback_video_edit::EditBoundary::In,
               "rendered edit handles must retain their boundary identity");
  const playback_overlay::InteractionMap transformed =
      playback_overlay::transformInteractionMap(interactions, 5.0, 7.0,
                                                2.0, 3.0);
  ok &= expect(playback_overlay::overlayControlAt(transformed, 50.0, 100.0) ==
                   playback_overlay::OverlayControlId::EditStartExport,
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

  contextMenu.selectedItem = 40;
  contextMenu.items = {
      {10, "First"},
      {20, "Second"},
      {30, "Third"},
      {40, "Last selected command"},
  };
  const playback_overlay::ContextMenuCellLayout scrolledContextLayout =
      playback_overlay::layoutContextMenuCells(contextMenu, 30, 4);
  ok &= expect(scrolledContextLayout.items.size() == 2 &&
                   scrolledContextLayout.items[0].token == 30 &&
                   scrolledContextLayout.items[1].token == 40 &&
                   scrolledContextLayout.items[1].selected,
               "a height-limited context menu must keep keyboard selection "
               "inside its visible viewport");

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
  ok &= expect(terminalMenu.visible && terminalMenu.items.size() == 2 &&
                   terminalMenu.items[0].label == "Edit video" &&
                   terminalMenu.items[1].label ==
                       "Generate subtitles..." &&
                   terminalMenu.items[0].token != 0 &&
                   terminalMenu.items[1].token != 0 &&
                   terminalMenu.items[0].token !=
                       terminalMenu.items[1].token &&
                   !windowMenu.visible,
               "a playback context menu must expose unique opaque source "
               "action identities on exactly one presentation surface");
  cleanEdit.hasEdits = true;
  playbackMenu.refresh(cleanEdit, idleExport);
  const auto retainedMenu = playbackMenu.snapshotFor(
      playback_session::ContextMenuSurface::Terminal);
  ok &= expect(retainedMenu.items.size() == 4 &&
                   retainedMenu.items[0].label == "Resume editing" &&
                   retainedMenu.items[1].label ==
                       "Generate subtitles..." &&
                   retainedMenu.items[2].label == "Export edited copy" &&
                   retainedMenu.items[3].label == "Discard changes" &&
                   retainedMenu.items[0].token ==
                       terminalMenu.items[0].token &&
                   retainedMenu.items[1].token ==
                       terminalMenu.items[1].token,
               "an exported edit revision must remain resumable, exportable, "
               "transcribable, and discardable without changing semantic "
               "item identity");
  cleanEdit.active = true;
  cleanEdit.hasUnexportedChanges = true;
  cleanEdit.inTimelineUs = 1'000'000;
  cleanEdit.outTimelineUs = 2'000'000;
  cleanEdit.canRippleDelete = true;
  cleanEdit.canTrim = true;
  cleanEdit.canUndo = true;
  cleanEdit.canRedo = true;
  cleanEdit.canToggleSmoothCut = true;
  cleanEdit.selectedCutTransition =
      playback_video_edit::CutTransition::hard();
  playbackMenu.refresh(cleanEdit, idleExport);
  const auto dirtyMenu = playbackMenu.snapshotFor(
      playback_session::ContextMenuSurface::Terminal);
  const auto clearAllItem = std::find_if(
      dirtyMenu.items.begin(), dirtyMenu.items.end(), [](const auto& item) {
        return item.label == "Cancel selection";
      });
  const auto removeItem = std::find_if(
      dirtyMenu.items.begin(), dirtyMenu.items.end(), [](const auto& item) {
        return item.label == "Remove selected section";
      });
  const auto keepOnlyItem = std::find_if(
      dirtyMenu.items.begin(), dirtyMenu.items.end(), [](const auto& item) {
        return item.label == "Keep only selected section";
      });
  const auto undoItem = std::find_if(
      dirtyMenu.items.begin(), dirtyMenu.items.end(), [](const auto& item) {
        return item.label == "Undo";
      });
  const auto redoItem = std::find_if(
      dirtyMenu.items.begin(), dirtyMenu.items.end(), [](const auto& item) {
        return item.label == "Redo";
      });
  const auto resetItem = std::find_if(
      dirtyMenu.items.begin(), dirtyMenu.items.end(), [](const auto& item) {
        return item.label == "Reset all edits";
      });
  const auto doneItem = std::find_if(
      dirtyMenu.items.begin(), dirtyMenu.items.end(), [](const auto& item) {
        return item.label == "Done editing";
      });
  const auto discardItem = std::find_if(
      dirtyMenu.items.begin(), dirtyMenu.items.end(), [](const auto& item) {
        return item.label == "Discard changes";
      });
  const auto exportItem = std::find_if(
      dirtyMenu.items.begin(), dirtyMenu.items.end(), [](const auto& item) {
        return item.label == "Export edited copy";
      });
  const auto smoothCutItem = std::find_if(
      dirtyMenu.items.begin(), dirtyMenu.items.end(), [](const auto& item) {
        return item.label == "Smooth cut";
      });
  const auto detectSegmentsItem = std::find_if(
      dirtyMenu.items.begin(), dirtyMenu.items.end(), [](const auto& item) {
        return item.label == "Detect segments...";
      });
  ok &= expect(clearAllItem != dirtyMenu.items.end() &&
                   removeItem != dirtyMenu.items.end() &&
                   keepOnlyItem != dirtyMenu.items.end() &&
                   undoItem != dirtyMenu.items.end() &&
                   redoItem != dirtyMenu.items.end() &&
                   resetItem != dirtyMenu.items.end() &&
                   doneItem != dirtyMenu.items.end() &&
                   discardItem != dirtyMenu.items.end() &&
                   exportItem != dirtyMenu.items.end() &&
                   smoothCutItem != dirtyMenu.items.end() &&
                   detectSegmentsItem != dirtyMenu.items.end(),
               "the context menu must own secondary edit commands");
  cleanEdit.sceneAnalysisStatus =
      playback_video_edit::SceneAnalysisStatus::Ready;
  cleanEdit.suggestionReview.visible = true;
  cleanEdit.suggestionReview.canUndoHide = true;
  playback_video_edit::SceneSuggestionSnapshot menuSuggestion;
  menuSuggestion.id = 9;
  menuSuggestion.selected = true;
  menuSuggestion.spans.push_back({0, 1'000'000});
  cleanEdit.suggestionReview.suggestions = {menuSuggestion};
  cleanEdit.suggestionReview.selectedId = menuSuggestion.id;
  playbackMenu.refresh(cleanEdit, idleExport);
  const auto analysedMenu = playbackMenu.snapshotFor(
      playback_session::ContextMenuSurface::Terminal);
  ok &= expect(std::any_of(
                   analysedMenu.items.begin(), analysedMenu.items.end(),
                   [](const auto& item) {
                     return item.label == "Select suggested segment";
                   }) &&
                   std::any_of(
                       analysedMenu.items.begin(), analysedMenu.items.end(),
                       [](const auto& item) {
                         return item.label == "Hide suggestion";
                       }) &&
                   std::any_of(
                       analysedMenu.items.begin(), analysedMenu.items.end(),
                       [](const auto& item) {
                         return item.label == "Undo hidden suggestion";
                       }),
               "detected ranges must expose deliberate selection, reversible "
               "hiding, and no destructive edit");
  cleanEdit.selectedCutTransition =
      playback_video_edit::CutTransition::motionSmooth();
  playbackMenu.refresh(cleanEdit, idleExport);
  const auto smoothEnabledMenu = playbackMenu.snapshotFor(
      playback_session::ContextMenuSurface::Terminal);
  ok &= expect(std::any_of(
                   smoothEnabledMenu.items.begin(),
                   smoothEnabledMenu.items.end(), [](const auto& item) {
                     return item.label == "Hard cut";
                   }),
               "an enabled smooth transition must expose its inverse action");
  const auto staleStartExportToken =
      exportItem != dirtyMenu.items.end()
          ? std::optional<playback_overlay::ContextMenuItemToken>(
                exportItem->token)
          : std::nullopt;
  const auto isEditCommand = [](const auto& command,
                                playback_video_edit::Command expected) {
    if (!command) return false;
    const auto* edit =
        std::get_if<playback_video_edit::Command>(&*command);
    return edit && *edit == expected;
  };
  if (doneItem != dirtyMenu.items.end()) {
    const bool selectedDone = playbackMenu.select(doneItem->token);
    const auto activatedDone = playbackMenu.activateSelection();
    ok &= expect(selectedDone &&
                     isEditCommand(activatedDone,
                                   playback_video_edit::Command::Finish) &&
                     !playbackMenu.visible(),
                 "the context Done action must finish without entering the "
                 "Escape confirmation path or starting an implicit export");
  }
  ok &= expect(playbackMenu.open(
                   playback_session::ContextMenuSurface::Terminal, 0.25,
                   0.75),
               "the context menu must reopen after executing a command");
  if (discardItem != dirtyMenu.items.end()) {
    const bool selectedDiscard = playbackMenu.select(discardItem->token);
    const auto activatedDiscard = playbackMenu.activateSelection();
    ok &= expect(
        selectedDiscard &&
            isEditCommand(activatedDiscard,
                          playback_video_edit::Command::RequestDiscard),
                 "context discard must still request confirmation");
  }

  playback_video_edit::ExportProgress runningExport;
  runningExport.status = playback_video_edit::ExportStatus::Running;
  playbackMenu.refresh(cleanEdit, runningExport);
  ok &= expect(playbackMenu.open(
                   playback_session::ContextMenuSurface::Terminal, 0.25,
                   0.75),
               "a running export must retain a secondary command surface");
  const auto runningExportMenu = playbackMenu.snapshotFor(
      playback_session::ContextMenuSurface::Terminal);
  ok &= expect(std::any_of(
                   runningExportMenu.items.begin(),
                   runningExportMenu.items.end(), [](const auto& item) {
                     return item.label == "Cancel older export";
                   }),
               "an older background job must expose precise cancellation "
               "without claiming to contain newer edits");
  ok &= expect(std::any_of(
                   runningExportMenu.items.begin(),
                   runningExportMenu.items.end(), [](const auto& item) {
                     return item.label == "Done editing";
                   }),
                "leaving the edit tools must remain independent from an older "
                "background export");
  if (staleStartExportToken) {
    ok &= expect(!playbackMenu.activate(*staleStartExportToken) &&
                     playbackMenu.visible(),
                 "a stale start-export hit target must never cancel a job "
                 "that started after the menu was rendered");
  }

  playbackMenu.dismiss();
  playbackMenu.refresh(cleanEdit, failedExport);
  ok &= expect(playbackMenu.open(
                   playback_session::ContextMenuSurface::Terminal, 0.25,
                   0.75),
               "a failed current-revision export must remain actionable");
  const auto failedExportMenu = playbackMenu.snapshotFor(
      playback_session::ContextMenuSurface::Terminal);
  ok &= expect(std::any_of(
                   failedExportMenu.items.begin(), failedExportMenu.items.end(),
                   [](const auto& item) {
                     return item.label == "Retry export";
                   }) &&
                   std::none_of(
                       failedExportMenu.items.begin(),
                       failedExportMenu.items.end(), [](const auto& item) {
                         return item.label == "Cancel export";
                       }),
               "a terminal export failure must offer retry rather than "
               "pretending that a worker is still active");

  return ok ? 0 : 1;
}
