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
  return sourceDurationUs_ > 0 && keptRanges_.size() == 1 &&
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
  return rangeContaining(sourceUs).has_value();
}

std::optional<SourceRange> Timeline::rangeContaining(int64_t sourceUs) const {
  for (const SourceRange& range : keptRanges_) {
    if (sourceUs >= range.startUs && sourceUs < range.endUs) return range;
  }
  return std::nullopt;
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

std::optional<int64_t> Timeline::sourceToOutputTime(int64_t sourceUs) const {
  int64_t outputUs = 0;
  for (const SourceRange& range : keptRanges_) {
    if (sourceUs >= range.startUs && sourceUs < range.endUs) {
      return saturatingAdd(outputUs, sourceUs - range.startUs);
    }
    outputUs = saturatingAdd(outputUs, range.durationUs());
  }
  if (!keptRanges_.empty() && sourceUs == keptRanges_.back().endUs) {
    return outputUs;
  }
  return std::nullopt;
}

std::optional<int64_t> Timeline::outputToSourceTime(int64_t outputUs) const {
  if (outputUs < 0) return std::nullopt;
  int64_t remaining = outputUs;
  for (const SourceRange& range : keptRanges_) {
    if (remaining < range.durationUs()) return range.startUs + remaining;
    remaining -= range.durationUs();
  }
  if (!keptRanges_.empty() && remaining == 0) {
    return keptRanges_.back().endUs;
  }
  return std::nullopt;
}

void EditSession::activate(int64_t sourceDurationUs) {
  const int64_t duration = std::max<int64_t>(0, sourceDurationUs);
  bool changed = false;
  if (timeline_.sourceDurationUs() != duration) {
    timeline_.reset(duration);
    undo_.clear();
    redo_.clear();
    inUs_.reset();
    outUs_.reset();
    changed = true;
  }
  const bool nextActive = duration > 0;
  changed = changed || active_ != nextActive;
  active_ = nextActive;
  if (changed) ++revision_;
}

void EditSession::deactivate() {
  if (!active_ && !inUs_ && !outUs_) return;
  active_ = false;
  inUs_.reset();
  outUs_.reset();
  ++revision_;
}

int64_t EditSession::clampSourceTime(int64_t sourceUs) const {
  return std::clamp(sourceUs, int64_t{0}, timeline_.sourceDurationUs());
}

void EditSession::markIn(int64_t sourceUs) {
  if (!active_) return;
  const int64_t nextIn = clampSourceTime(sourceUs);
  const bool clearsOut = outUs_ && *outUs_ <= nextIn;
  if (inUs_ == nextIn && !clearsOut) return;
  inUs_ = nextIn;
  if (clearsOut) outUs_.reset();
  ++revision_;
}

void EditSession::markOut(int64_t sourceUsExclusive) {
  if (!active_) return;
  const int64_t nextOut = clampSourceTime(sourceUsExclusive);
  const bool clearsIn = inUs_ && *inUs_ >= nextOut;
  if (outUs_ == nextOut && !clearsIn) return;
  outUs_ = nextOut;
  if (clearsIn) inUs_.reset();
  ++revision_;
}

void EditSession::clearSelection() {
  if (!inUs_ && !outUs_) return;
  inUs_.reset();
  outUs_.reset();
  ++revision_;
}

std::optional<SourceRange> EditSession::selection() const {
  if (!inUs_ || !outUs_ || *outUs_ <= *inUs_) return std::nullopt;
  return SourceRange{*inUs_, *outUs_};
}

bool EditSession::commit(Timeline next) {
  if (next.keptRanges() == timeline_.keptRanges()) return false;
  undo_.push_back(timeline_);
  timeline_ = std::move(next);
  redo_.clear();
  inUs_.reset();
  outUs_.reset();
  ++revision_;
  return true;
}

bool EditSession::trimToSelection() {
  const std::optional<SourceRange> selected = selection();
  if (!active_ || !selected) return false;
  Timeline next = timeline_;
  return next.trimTo(*selected) && commit(std::move(next));
}

bool EditSession::rippleDeleteSelection() {
  const std::optional<SourceRange> selected = selection();
  if (!active_ || !selected) return false;
  Timeline next = timeline_;
  return next.rippleDelete(*selected) && commit(std::move(next));
}

bool EditSession::undo() {
  if (!active_ || undo_.empty()) return false;
  redo_.push_back(timeline_);
  timeline_ = std::move(undo_.back());
  undo_.pop_back();
  inUs_.reset();
  outUs_.reset();
  ++revision_;
  return true;
}

bool EditSession::redo() {
  if (!active_ || redo_.empty()) return false;
  undo_.push_back(timeline_);
  timeline_ = std::move(redo_.back());
  redo_.pop_back();
  inUs_.reset();
  outUs_.reset();
  ++revision_;
  return true;
}

bool EditSession::resetEdits() {
  if (!active_ || timeline_.isUnmodified()) return false;
  Timeline next(timeline_.sourceDurationUs());
  return commit(std::move(next));
}

EditSnapshot EditSession::snapshot() const {
  EditSnapshot out;
  out.active = active_;
  out.sourceDurationUs = timeline_.sourceDurationUs();
  out.outputDurationUs = timeline_.outputDurationUs();
  out.keptRanges = timeline_.keptRanges();
  out.inUs = inUs_;
  out.outUs = outUs_;
  out.canApplySelection = selection().has_value();
  out.canUndo = !undo_.empty();
  out.canRedo = !redo_.empty();
  out.revision = revision_;
  return out;
}

}  // namespace playback_video_edit
