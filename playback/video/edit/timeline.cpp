#include "playback/video/edit/timeline.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <utility>

namespace playback_video_edit {
namespace {

bool validRange(SourceRange range) {
  return range.startUs >= 0 && range.endUs > range.startUs;
}

SourceRange clampRange(SourceRange range, int64_t durationUs) {
  range.startUs = std::clamp(range.startUs, int64_t{0}, durationUs);
  range.endUs = std::clamp(range.endUs, int64_t{0}, durationUs);
  return range;
}

int64_t saturatingAdd(int64_t lhs, int64_t rhs) {
  if (rhs > 0 && lhs > (std::numeric_limits<int64_t>::max)() - rhs) {
    return (std::numeric_limits<int64_t>::max)();
  }
  return lhs + rhs;
}

}  // namespace

Timeline::Timeline(int64_t sourceDurationUs) { reset(sourceDurationUs); }

void Timeline::reset(int64_t sourceDurationUs) {
  sourceDurationUs_ = std::max<int64_t>(0, sourceDurationUs);
  keptRanges_.clear();
  cutTransitions_.clear();
  if (sourceDurationUs_ > 0) {
    keptRanges_.push_back(SourceRange{0, sourceDurationUs_});
  }
}

int64_t Timeline::outputDurationUs() const {
  int64_t total = 0;
  for (const SourceRange& range : keptRanges_) {
    total = saturatingAdd(total, range.durationUs());
  }
  return total;
}

bool Timeline::isUnmodified() const {
  if (sourceDurationUs_ <= 0) return keptRanges_.empty();
  return cutTransitions_.empty() && keptRanges_.size() == 1 &&
         keptRanges_.front() == SourceRange{0, sourceDurationUs_};
}

bool Timeline::replaceRanges(std::vector<SourceRange> ranges) {
  const std::vector<SourceRange> previousRanges = keptRanges_;
  const std::vector<CutTransition> previousTransitions = cutTransitions_;
  std::vector<SourceRange> normalized;
  normalized.reserve(ranges.size());
  for (SourceRange range : ranges) {
    range.startUs = std::clamp(range.startUs, int64_t{0}, sourceDurationUs_);
    range.endUs = std::clamp(range.endUs, int64_t{0}, sourceDurationUs_);
    if (!validRange(range)) continue;
    if (!normalized.empty() && range.startUs <= normalized.back().endUs) {
      normalized.back().endUs =
          std::max(normalized.back().endUs, range.endUs);
    } else {
      normalized.push_back(range);
    }
  }
  if (normalized.empty()) return false;
  std::vector<CutTransition> transitions(
      normalized.size() > 1 ? normalized.size() - 1 : 0,
      CutTransition::hard());
  for (size_t next = 0; next < transitions.size(); ++next) {
    for (size_t previous = 0; previous < previousTransitions.size();
         ++previous) {
      if (previousRanges[previous].endUs == normalized[next].endUs &&
          previousRanges[previous + 1].startUs ==
              normalized[next + 1].startUs) {
        transitions[next] = previousTransitions[previous];
        break;
      }
    }
  }
  keptRanges_ = std::move(normalized);
  cutTransitions_ = std::move(transitions);
  return true;
}

bool Timeline::canTrimTo(SourceRange keep) const {
  keep = clampRange(keep, sourceDurationUs_);
  if (!validRange(keep)) return false;

  bool retainsVideo = false;
  bool changesSequence = false;
  for (const SourceRange& range : keptRanges_) {
    const SourceRange intersection{std::max(range.startUs, keep.startUs),
                                   std::min(range.endUs, keep.endUs)};
    retainsVideo = retainsVideo || validRange(intersection);
    changesSequence = changesSequence || intersection != range;
  }
  return retainsVideo && changesSequence;
}

bool Timeline::trimTo(SourceRange keep) {
  if (!canTrimTo(keep)) return false;
  keep = clampRange(keep, sourceDurationUs_);

  std::vector<SourceRange> next;
  next.reserve(keptRanges_.size());
  for (const SourceRange& range : keptRanges_) {
    SourceRange intersection{std::max(range.startUs, keep.startUs),
                             std::min(range.endUs, keep.endUs)};
    if (validRange(intersection)) next.push_back(intersection);
  }
  if (next.empty() || next == keptRanges_) return false;
  return replaceRanges(std::move(next));
}

bool Timeline::canRippleDelete(SourceRange remove) const {
  remove = clampRange(remove, sourceDurationUs_);
  if (!validRange(remove)) return false;

  bool removesVideo = false;
  bool retainsVideo = false;
  for (const SourceRange& range : keptRanges_) {
    if (remove.endUs <= range.startUs || remove.startUs >= range.endUs) {
      retainsVideo = true;
      continue;
    }
    removesVideo = true;
    retainsVideo = retainsVideo || range.startUs < remove.startUs ||
                   remove.endUs < range.endUs;
  }
  return removesVideo && retainsVideo;
}

bool Timeline::rippleDelete(SourceRange remove) {
  if (!canRippleDelete(remove)) return false;
  remove = clampRange(remove, sourceDurationUs_);

  std::vector<SourceRange> next;
  next.reserve(keptRanges_.size() + 1);
  for (const SourceRange& range : keptRanges_) {
    if (remove.endUs <= range.startUs || remove.startUs >= range.endUs) {
      next.push_back(range);
      continue;
    }
    if (range.startUs < remove.startUs) {
      next.push_back(SourceRange{range.startUs,
                                 std::min(remove.startUs, range.endUs)});
    }
    if (remove.endUs < range.endUs) {
      next.push_back(
          SourceRange{std::max(remove.endUs, range.startUs), range.endUs});
    }
  }
  if (next.empty() || next == keptRanges_) return false;
  return replaceRanges(std::move(next));
}

std::optional<size_t> Timeline::nearestCutIndex(int64_t timelineUs,
                                                int64_t toleranceUs) const {
  if (cutTransitions_.empty() || toleranceUs < 0) return std::nullopt;
  timelineUs = std::clamp(timelineUs, int64_t{0}, outputDurationUs());
  int64_t cutUs = 0;
  std::optional<size_t> nearest;
  int64_t nearestDistance = (std::numeric_limits<int64_t>::max)();
  for (size_t index = 0; index < cutTransitions_.size(); ++index) {
    cutUs = saturatingAdd(cutUs, keptRanges_[index].durationUs());
    const int64_t distance = std::llabs(timelineUs - cutUs);
    if (distance <= toleranceUs && distance < nearestDistance) {
      nearest = index;
      nearestDistance = distance;
    }
  }
  return nearest;
}

bool Timeline::setCutTransition(size_t cutIndex,
                                CutTransition transition) {
  if (cutIndex >= cutTransitions_.size()) return false;
  transition = transition.kind == CutTransitionKind::MotionSmooth
                   ? CutTransition::motionSmooth(transition.durationFrames())
                   : CutTransition::hard();
  if (cutTransitions_[cutIndex] == transition) return false;
  cutTransitions_[cutIndex] = transition;
  return true;
}

bool Timeline::containsSourceTime(int64_t sourceUs) const {
  for (const SourceRange& range : keptRanges_) {
    if (sourceUs >= range.startUs && sourceUs < range.endUs) return true;
  }
  return false;
}

std::optional<int64_t> Timeline::nextKeptSourceTime(int64_t sourceUs) const {
  for (const SourceRange& range : keptRanges_) {
    if (sourceUs < range.startUs) return range.startUs;
    if (sourceUs < range.endUs) return std::max(sourceUs, range.startUs);
  }
  return std::nullopt;
}

std::optional<int64_t> Timeline::previousKeptSourceTime(int64_t sourceUs) const {
  for (auto it = keptRanges_.rbegin(); it != keptRanges_.rend(); ++it) {
    if (sourceUs > it->endUs) return it->endUs - 1;
    if (sourceUs > it->startUs) return std::min(sourceUs - 1, it->endUs - 1);
  }
  return std::nullopt;
}

void Document::load(int64_t sourceDurationUs) {
  const int64_t duration = std::max<int64_t>(0, sourceDurationUs);
  if (timeline_.sourceDurationUs() != duration) {
    timeline_.reset(duration);
    undo_.clear();
    redo_.clear();
    exportedRevisions_.clear();
  }
}

bool Document::discardAllChanges() {
  if (timeline_.isUnmodified()) return false;
  const int64_t duration = timeline_.sourceDurationUs();
  timeline_.reset(duration);
  undo_.clear();
  redo_.clear();
  return true;
}

bool Selection::clear() {
  if (!inUs_ && !outUs_) return false;
  inUs_.reset();
  outUs_.reset();
  outFrameUs_.reset();
  return true;
}

bool Selection::clear(EditBoundary boundary) {
  if (boundary == EditBoundary::In) {
    if (!inUs_) return false;
    inUs_.reset();
    return true;
  }
  if (!outUs_) return false;
  outUs_.reset();
  outFrameUs_.reset();
  return true;
}

void Selection::markIn(const Timeline& timeline, int64_t sourceUs,
                       int64_t minimumSelectionDurationUs) {
  int64_t nextIn = std::clamp(
      sourceUs, int64_t{0}, timeline.sourceDurationUs());
  if (outUs_) {
    const auto sequence = playback_video_sequence::Timeline::create(
        timeline.sourceDurationUs(), timeline.keptRanges());
    if (sequence) {
      const auto requested = sequence->pointForSource(
          nextIn, playback_video_sequence::SourceBias::Forward);
      const auto end = sequence->pointForSource(
          *outUs_, playback_video_sequence::SourceBias::Backward);
      if (requested && end) {
        const int64_t minimumDurationUs =
            std::max<int64_t>(1, minimumSelectionDurationUs);
        const int64_t latestStartUs =
            end->presentationUs -
            std::min(minimumDurationUs, end->presentationUs);
        nextIn = sequence->pointAt(
            std::min(requested->presentationUs, latestStartUs)).sourceUs;
      }
    }
  }
  if (inUs_ == nextIn) return;
  inUs_ = nextIn;
}

void Selection::markOut(const Timeline& timeline,
                        int64_t sourceFrameStartUs,
                        int64_t sourceFrameEndUs,
                        int64_t minimumSelectionDurationUs) {
  int64_t nextOut = std::clamp(
      sourceFrameEndUs, int64_t{0}, timeline.sourceDurationUs());
  int64_t nextOutFrame =
      std::clamp(sourceFrameStartUs, int64_t{0}, nextOut);
  if (inUs_) {
    const auto sequence = playback_video_sequence::Timeline::create(
        timeline.sourceDurationUs(), timeline.keptRanges());
    if (sequence) {
      const auto start = sequence->pointForSource(
          *inUs_, playback_video_sequence::SourceBias::Forward);
      const auto requested = sequence->pointForSource(
          nextOut, playback_video_sequence::SourceBias::Backward);
      if (start && requested) {
        const int64_t minimumDurationUs =
            std::max<int64_t>(1, minimumSelectionDurationUs);
        const int64_t earliestEndUs =
            start->presentationUs +
            std::min(minimumDurationUs,
                     sequence->durationUs() - start->presentationUs);
        const int64_t targetUs =
            std::max(requested->presentationUs, earliestEndUs);
        if (targetUs != requested->presentationUs) {
          if (targetUs <= 0) {
            nextOut = sequence->pointAt(0).sourceUs;
            nextOutFrame = nextOut;
          } else {
            const playback_video_sequence::Point beforeBoundary =
                sequence->pointAt(targetUs - 1);
            nextOut = std::min(timeline.sourceDurationUs(),
                               beforeBoundary.sourceUs + 1);
            nextOutFrame = beforeBoundary.sourceUs;
          }
        }
      }
    }
  }
  setOutBoundary(timeline, nextOut, nextOutFrame);
}

void Selection::setOutBoundary(const Timeline& timeline,
                               int64_t sourceUsExclusive,
                               int64_t sourceFrameStartUs) {
  const int64_t nextOut = std::clamp(
      sourceUsExclusive, int64_t{0}, timeline.sourceDurationUs());
  const int64_t nextOutFrame =
      std::clamp(sourceFrameStartUs, int64_t{0}, nextOut);
  if (outUs_ == nextOut && outFrameUs_ == nextOutFrame) return;
  outUs_ = nextOut;
  outFrameUs_ = nextOutFrame;
}

bool Selection::moveBoundary(const Timeline& timeline, EditBoundary boundary,
                             int64_t timelineUs,
                             int64_t minimumSelectionDurationUs) {
  const auto sequence = playback_video_sequence::Timeline::create(
      timeline.sourceDurationUs(), timeline.keptRanges());
  if (!sequence) return false;

  const int64_t durationUs = sequence->durationUs();
  int64_t targetUs = std::clamp(timelineUs, int64_t{0}, durationUs);
  const int64_t minimumDurationUs =
      std::max<int64_t>(1, minimumSelectionDurationUs);
  if (boundary == EditBoundary::In && outUs_) {
    const auto outPoint = sequence->pointForSource(
        *outUs_, playback_video_sequence::SourceBias::Backward);
    if (outPoint) {
      targetUs = std::min(
          targetUs,
          outPoint->presentationUs -
              std::min(minimumDurationUs, outPoint->presentationUs));
    }
  } else if (boundary == EditBoundary::Out && inUs_) {
    const auto inPoint = sequence->pointForSource(
        *inUs_, playback_video_sequence::SourceBias::Forward);
    if (inPoint) {
      targetUs = std::max(
          targetUs,
          inPoint->presentationUs +
              std::min(minimumDurationUs,
                       durationUs - inPoint->presentationUs));
    }
  }

  const std::optional<int64_t> previousIn = inUs_;
  const std::optional<int64_t> previousOut = outUs_;
  const std::optional<int64_t> previousOutFrame = outFrameUs_;
  if (boundary == EditBoundary::In) {
    markIn(timeline, sequence->pointAt(targetUs).sourceUs,
           minimumDurationUs);
  } else if (targetUs <= 0) {
    const int64_t sourceUs = sequence->pointAt(0).sourceUs;
    setOutBoundary(timeline, sourceUs, sourceUs);
  } else {
    const playback_video_sequence::Point beforeBoundary =
        sequence->pointAt(targetUs - 1);
    setOutBoundary(timeline,
                   std::min(timeline.sourceDurationUs(),
                            beforeBoundary.sourceUs + 1),
                   beforeBoundary.sourceUs);
  }
  return inUs_ != previousIn || outUs_ != previousOut ||
         outFrameUs_ != previousOutFrame;
}

std::optional<SourceRange> Selection::range() const {
  if (!inUs_ || !outUs_ || *outUs_ <= *inUs_) return std::nullopt;
  return SourceRange{*inUs_, *outUs_};
}

bool Document::commit(Timeline next) {
  if (next.decisionList() == timeline_.decisionList()) return false;
  undo_.push_back(timeline_);
  timeline_ = std::move(next);
  redo_.clear();
  return true;
}

bool Document::trimTo(SourceRange keep) {
  Timeline next = timeline_;
  return next.trimTo(keep) && commit(std::move(next));
}

bool Document::rippleDelete(SourceRange remove) {
  Timeline next = timeline_;
  return next.rippleDelete(remove) && commit(std::move(next));
}

bool Document::setCutTransition(size_t cutIndex,
                                CutTransition transition) {
  Timeline next = timeline_;
  return next.setCutTransition(cutIndex, transition) &&
         commit(std::move(next));
}

bool Document::undo() {
  if (undo_.empty()) return false;
  redo_.push_back(timeline_);
  timeline_ = std::move(undo_.back());
  undo_.pop_back();
  return true;
}

bool Document::redo() {
  if (redo_.empty()) return false;
  undo_.push_back(timeline_);
  timeline_ = std::move(redo_.back());
  redo_.pop_back();
  return true;
}

bool Document::resetEdits() {
  if (timeline_.isUnmodified()) return false;
  Timeline next(timeline_.sourceDurationUs());
  return commit(std::move(next));
}

bool Document::hasUnexportedChanges() const {
  if (timeline_.isUnmodified()) return false;
  return std::find(exportedRevisions_.begin(), exportedRevisions_.end(),
                   timeline_.decisionList()) == exportedRevisions_.end();
}

void Document::markExported(const DecisionList& decisions) {
  if (!decisions.hasValidShape()) return;
  if (std::find(exportedRevisions_.begin(), exportedRevisions_.end(),
                decisions) == exportedRevisions_.end()) {
    exportedRevisions_.push_back(decisions);
  }
}

EditSnapshot buildSnapshot(const Document& document,
                           const Selection& selection, bool active,
                           std::optional<int64_t> playheadTimelineUs,
                           int64_t timecodeFrameDurationUs) {
  const Timeline& timeline = document.timeline();
  EditSnapshot out;
  out.active = active;
  out.hasEdits = !timeline.isUnmodified();
  out.hasUnexportedChanges = document.hasUnexportedChanges();
  if (const auto remove = selection.range()) {
    out.canRippleDelete = timeline.canRippleDelete(*remove);
  }
  if (const auto keep = selection.range()) {
    out.canTrim = timeline.canTrimTo(*keep);
  }
  out.canUndo = document.canUndo();
  out.canRedo = document.canRedo();
  out.sourceDurationUs = timeline.sourceDurationUs();
  out.timelineDurationUs = timeline.outputDurationUs();
  out.timecodeFrameDurationUs =
      std::max<int64_t>(0, timecodeFrameDurationUs);
  out.keptRanges = timeline.keptRanges();
  out.inSourceUs = selection.inSourceUs();
  out.outSourceUs = selection.outSourceUs();
  out.playheadTimelineUs = active ? playheadTimelineUs : std::nullopt;

  const auto sequence = playback_video_sequence::Timeline::create(
      timeline.sourceDurationUs(), timeline.keptRanges());
  if (!sequence) return out;
  out.clips.reserve(sequence->clips().size());
  for (const playback_video_sequence::Clip& clip : sequence->clips()) {
    out.clips.push_back(EditClipSnapshot{clip.source,
                                         clip.presentationStartUs});
  }
  out.cuts.reserve(timeline.cutTransitions().size());
  for (size_t cut = 0; cut < timeline.cutTransitions().size(); ++cut) {
    out.cuts.push_back(
        EditCutSnapshot{sequence->clips()[cut].presentationEndUs(),
                        timeline.cutTransitions()[cut]});
  }
  if (active && playheadTimelineUs && !out.cuts.empty()) {
    const int64_t frameDurationUs =
        std::max<int64_t>(1, timecodeFrameDurationUs);
    const int64_t toleranceUs = std::max<int64_t>(
        50'000,
        frameDurationUs <= (std::numeric_limits<int64_t>::max)() / 2
            ? frameDurationUs * 2
            : frameDurationUs);
    if (const auto selected = timeline.nearestCutIndex(
            *playheadTimelineUs, toleranceUs)) {
      out.selectedCutTransition = timeline.cutTransitions()[*selected];
      out.canToggleSmoothCut = true;
    }
  }
  if (out.inSourceUs) {
    const auto point = sequence->pointForSource(
        *out.inSourceUs, playback_video_sequence::SourceBias::Forward);
    if (point) out.inTimelineUs = point->presentationUs;
  }
  if (out.outSourceUs) {
    const auto point = sequence->pointForSource(
        *out.outSourceUs, playback_video_sequence::SourceBias::Backward);
    if (point) out.outTimelineUs = point->presentationUs;
  }
  if (const auto outFrameSourceUs = selection.outFrameSourceUs()) {
    const auto point = sequence->pointForSource(
        *outFrameSourceUs, playback_video_sequence::SourceBias::Forward);
    if (point) out.outFrameTimelineUs = point->presentationUs;
  }
  return out;
}

}  // namespace playback_video_edit
