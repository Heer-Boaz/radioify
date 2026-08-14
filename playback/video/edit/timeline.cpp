#include "playback/video/edit/timeline.h"

#include <algorithm>
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
  return keptRanges_.size() == 1 &&
         keptRanges_.front() == SourceRange{0, sourceDurationUs_};
}

bool Timeline::replaceRanges(std::vector<SourceRange> ranges) {
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
  keptRanges_ = std::move(normalized);
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

void Selection::markIn(const Timeline& timeline, int64_t sourceUs) {
  const int64_t nextIn = std::clamp(
      sourceUs, int64_t{0}, timeline.sourceDurationUs());
  const bool clearsOut = outUs_ && *outUs_ <= nextIn;
  if (inUs_ == nextIn && !clearsOut) return;
  inUs_ = nextIn;
  if (clearsOut) {
    outUs_.reset();
    outFrameUs_.reset();
  }
}

void Selection::markOut(const Timeline& timeline,
                        int64_t sourceFrameStartUs,
                        int64_t sourceFrameEndUs) {
  setOutBoundary(timeline, sourceFrameEndUs, sourceFrameStartUs);
}

void Selection::setOutBoundary(const Timeline& timeline,
                               int64_t sourceUsExclusive,
                               int64_t sourceFrameStartUs) {
  const int64_t nextOut = std::clamp(
      sourceUsExclusive, int64_t{0}, timeline.sourceDurationUs());
  const int64_t nextOutFrame =
      std::clamp(sourceFrameStartUs, int64_t{0}, nextOut);
  const bool clearsIn = inUs_ && *inUs_ >= nextOut;
  if (outUs_ == nextOut && outFrameUs_ == nextOutFrame && !clearsIn) return;
  outUs_ = nextOut;
  outFrameUs_ = nextOutFrame;
  if (clearsIn) inUs_.reset();
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
    markIn(timeline, sequence->pointAt(targetUs).sourceUs);
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

std::optional<SourceRange> Selection::trimRange(
    const Timeline& timeline) const {
  if (!hasMarks()) return std::nullopt;
  const int64_t startUs = std::clamp(
      inUs_.value_or(0), int64_t{0}, timeline.sourceDurationUs());
  const int64_t endUs = std::clamp(
      outUs_.value_or(timeline.sourceDurationUs()), int64_t{0},
      timeline.sourceDurationUs());
  if (endUs <= startUs) return std::nullopt;
  return SourceRange{startUs, endUs};
}

bool Document::commit(Timeline next) {
  if (next.keptRanges() == timeline_.keptRanges()) return false;
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
                   timeline_.keptRanges()) == exportedRevisions_.end();
}

void Document::markExported(const std::vector<SourceRange>& ranges) {
  if (std::find(exportedRevisions_.begin(), exportedRevisions_.end(), ranges) ==
      exportedRevisions_.end()) {
    exportedRevisions_.push_back(ranges);
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
  if (const auto keep = selection.trimRange(timeline)) {
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
