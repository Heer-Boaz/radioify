#include "playback/video/edit/timeline.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace playback_video_edit {
namespace {

bool validRange(SourceRange range) {
  return range.startUs >= 0 && range.endUs > range.startUs;
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

bool Timeline::trimTo(SourceRange keep) {
  keep.startUs = std::clamp(keep.startUs, int64_t{0}, sourceDurationUs_);
  keep.endUs = std::clamp(keep.endUs, int64_t{0}, sourceDurationUs_);
  if (!validRange(keep)) return false;

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

bool Timeline::rippleDelete(SourceRange remove) {
  remove.startUs = std::clamp(remove.startUs, int64_t{0}, sourceDurationUs_);
  remove.endUs = std::clamp(remove.endUs, int64_t{0}, sourceDurationUs_);
  if (!validRange(remove)) return false;

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
    exportedRanges_.reset();
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

void Selection::clear() {
  inUs_.reset();
  outUs_.reset();
}

void Selection::markIn(const Timeline& timeline, int64_t sourceUs) {
  const int64_t nextIn = std::clamp(
      sourceUs, int64_t{0}, timeline.sourceDurationUs());
  const bool clearsOut = outUs_ && *outUs_ <= nextIn;
  if (inUs_ == nextIn && !clearsOut) return;
  inUs_ = nextIn;
  if (clearsOut) outUs_.reset();
}

void Selection::markOut(const Timeline& timeline,
                        int64_t sourceUsExclusive) {
  const int64_t nextOut = std::clamp(
      sourceUsExclusive, int64_t{0}, timeline.sourceDurationUs());
  const bool clearsIn = inUs_ && *inUs_ >= nextOut;
  if (outUs_ == nextOut && !clearsIn) return;
  outUs_ = nextOut;
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
  if (boundary == EditBoundary::In) {
    markIn(timeline, sequence->pointAt(targetUs).sourceUs);
  } else if (targetUs <= 0) {
    markOut(timeline, sequence->pointAt(0).sourceUs);
  } else {
    const playback_video_sequence::Point beforeBoundary =
        sequence->pointAt(targetUs - 1);
    markOut(timeline, std::min(timeline.sourceDurationUs(),
                               beforeBoundary.sourceUs + 1));
  }
  return inUs_ != previousIn || outUs_ != previousOut;
}

std::optional<SourceRange> Selection::range() const {
  if (!inUs_ || !outUs_ || *outUs_ <= *inUs_) return std::nullopt;
  return SourceRange{*inUs_, *outUs_};
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
  return !timeline_.isUnmodified() &&
         (!exportedRanges_ || timeline_.keptRanges() != *exportedRanges_);
}

void Document::markExported(const std::vector<SourceRange>& ranges) {
  exportedRanges_ = ranges;
}

EditSnapshot buildSnapshot(const Document& document,
                           const Selection& selection, bool active,
                           std::optional<int64_t> playheadTimelineUs,
                           int64_t frameDurationUs) {
  const Timeline& timeline = document.timeline();
  EditSnapshot out;
  out.active = active;
  out.hasEdits = !timeline.isUnmodified();
  out.hasUnexportedChanges = document.hasUnexportedChanges();
  out.canUndo = document.canUndo();
  out.canRedo = document.canRedo();
  out.sourceDurationUs = timeline.sourceDurationUs();
  out.timelineDurationUs = timeline.outputDurationUs();
  out.frameDurationUs = std::max<int64_t>(0, frameDurationUs);
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
  return out;
}

}  // namespace playback_video_edit
