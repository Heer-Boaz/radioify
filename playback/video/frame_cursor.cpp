#include "frame_cursor.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>

#include "queues.h"

namespace playback_video_frame_cursor {
namespace {

int64_t frameEndUs(const PresentedFrame& frame) {
  if (frame.durationUs <= 0 ||
      frame.ptsUs > (std::numeric_limits<int64_t>::max)() - frame.durationUs) {
    return frame.ptsUs;
  }
  return frame.ptsUs + frame.durationUs;
}

int64_t presentationFrameEndUs(
    const playback_video_frame_step_prefetch::SourceFrame& frame) {
  if (frame.durationUs <= 0 ||
      frame.ptsUs >
          (std::numeric_limits<int64_t>::max)() - frame.durationUs) {
    return frame.ptsUs;
  }
  return frame.ptsUs + frame.durationUs;
}

bool samePrefetchRequest(
    const playback_video_frame_step_prefetch::Request& left,
    const playback_video_frame_step_prefetch::Request& right) {
  return left.serial == right.serial && left.direction == right.direction &&
         left.rangeStartUs == right.rangeStartUs &&
         left.rangeEndUs == right.rangeEndUs &&
         playback_video_frame_step_prefetch::sameIdentity(
             left.boundary.identity, right.boundary.identity) &&
         playback_video_frame_step_prefetch::sameIdentity(left.join.identity,
                                                          right.join.identity);
}

}  // namespace

void Controller::PendingFrameStepSeek::begin(
    const playback_video_frame_step_seek::Plan& plan) {
  assert(plan.valid());
  target = plan.target;
  mode = plan.mode;
  prerollLogicalIndex = plan.anchor.logicalIndex;
  generation = plan.generation;
  direction = plan.direction;
  targetReady = false;
  backstepCandidate.reset();
}

void Controller::PendingFrameStepSeek::clear() {
  target.reset();
  mode = playback_video_frame_step_seek::PlanMode::ExactFrame;
  prerollLogicalIndex = 0;
  generation = 0;
  direction = playback_video_frame_step::Direction::Next;
  targetReady = false;
  backstepCandidate.reset();
}

void Controller::resetForSerial(
    int serial, const playback_video_frame_step_seek::Plan* seekPlan) {
  assert(serial > 0);
  entries_.clear();
  cursorIndex_ = 0;
  serial_ = serial;
  failedPrefetchRequest_.reset();
  if (serial == 1) {
    sourceCache_.clear();
    sourceStartIdentity_.reset();
    sourceEndIdentity_.reset();
  }
  if (seekPlan) {
    frameStepMode_ = true;
    pendingFrameStepSeek_.begin(*seekPlan);
    publishReplayPending(true);
    return;
  }

  frameStepMode_ = false;
  pendingFrameStepSeek_.clear();
  records_.clear();
  currentLogicalIndex_ = 0;
  publishReplayPending(false);
}

bool Controller::enterFrameStepMode() {
  const bool activated = !frameStepMode_;
  frameStepMode_ = true;
  if (activated && !entries_.empty() && cursorIndex_ < entries_.size()) {
    const auto currentIdentity = identityFor(entries_[cursorIndex_]);
    refreshFrameStepView(currentIdentity);
  }
  return activated;
}

bool Controller::adoptPrefetchedResult(
    playback_video_frame_step_prefetch::Result result) {
  const playback_video_frame_step_prefetch::Request& request = result.request;
  int64_t maximumRangeDurationUs =
      playback_video_frame_step_prefetch::kWindowDurationUs +
      playback_video_frame_step_prefetch::kRefillLeadDurationUs;
  if (request.boundary.durationUs > 0) {
    if (request.boundary.durationUs >
        (std::numeric_limits<int64_t>::max)() - maximumRangeDurationUs) {
      maximumRangeDurationUs = (std::numeric_limits<int64_t>::max)();
    } else {
      maximumRangeDurationUs += request.boundary.durationUs;
    }
  }
  if (!frameStepMode_ || pendingFrameStepSeek_.active() ||
      request.serial != serial_ || entries_.empty() || !request.valid() ||
      request.rangeEndUs - request.rangeStartUs > maximumRangeDurationUs ||
      result.frames.size() >
          playback_video_frame_step_prefetch::kMaxCachedFrameCount) {
    return false;
  }

  for (const auto& source : result.frames) {
    if (!source || source->ptsUs < 0 || source->sourcePtsUs < 0 ||
        source->durationUs <= 0 || source->sourceDurationUs <= 0) {
      return false;
    }
    if (request.direction == playback_video_frame_step::Direction::Previous &&
        source->ptsUs > request.join.ptsUs) {
      return false;
    }
    if (request.direction == playback_video_frame_step::Direction::Next &&
        source->ptsUs < request.join.ptsUs) {
      return false;
    }
  }

  const playback_video_frame_step_prefetch::FrameIdentity currentIdentity =
      identityFor(entries_[cursorIndex_]);
  playback_video_frame_step_prefetch::SourceFrameCache committedCache =
      sourceCache_;
  if (!committedCache.commitDecodedRun(request.direction, request.join.identity,
                                       std::move(result.joinFrame),
                                       result.frames) ||
      !committedCache
           .windowAround(
               currentIdentity,
               playback_video_frame_step_prefetch::kWindowDurationUs +
                   playback_video_frame_step_prefetch::kRefillLeadDurationUs,
               playback_video_frame_step_prefetch::kWindowDurationUs +
                   playback_video_frame_step_prefetch::kRefillLeadDurationUs,
               playback_video_frame_step_prefetch::kMaxCachedFrameCount)
           .valid()) {
    return false;
  }
  sourceCache_ = std::move(committedCache);

  if (result.reachedMediaBoundary) {
    playback_video_frame_step_prefetch::FrameIdentity terminal =
        request.join.identity;
    if (!result.frames.empty()) {
      terminal =
          request.direction == playback_video_frame_step::Direction::Previous
              ? result.frames.front()->identity
              : result.frames.back()->identity;
    }
    if (request.direction == playback_video_frame_step::Direction::Previous) {
      sourceStartIdentity_ = terminal;
    } else {
      sourceEndIdentity_ = terminal;
    }
  }

  if (!refreshFrameStepView(currentIdentity)) {
    assert(false && "Validated source cache must rebuild its cursor view");
    std::abort();
  }
  failedPrefetchRequest_.reset();
  publishReplayPending(!atNewestFrame());
  return true;
}

PrefetchWindow Controller::prefetchWindow() const {
  PrefetchWindow window;
  if (!frameStepMode_ || entries_.empty() || cursorIndex_ >= entries_.size()) {
    return window;
  }

  const PresentedFrame& current = entries_[cursorIndex_];
  window.current = boundaryFor(current);
  window.beforeEdge = boundaryFor(entries_.front());
  window.afterEdge = boundaryFor(entries_.back());
  window.beforeFrameCount = cursorIndex_;
  window.afterFrameCount = entries_.size() - cursorIndex_ - 1;
  window.beforeDurationUs =
      (std::max)(int64_t{0}, current.ptsUs - entries_.front().ptsUs);
  window.afterDurationUs =
      (std::max)(int64_t{0}, frameEndUs(entries_.back()) - frameEndUs(current));
  return window;
}

std::optional<playback_video_frame_step_prefetch::Request>
Controller::prefetchRequest(playback_video_frame_step::Direction direction,
                            int64_t mediaDurationUs, int64_t horizonUs) {
  if (!frameStepMode_ || pendingFrameStepSeek_.active() || entries_.empty() ||
      cursorIndex_ >= entries_.size() || horizonUs < 0 ||
      horizonUs > playback_video_frame_step_prefetch::kWindowDurationUs) {
    return std::nullopt;
  }

  playback_video_frame_step_prefetch::FrameIdentity currentIdentity =
      identityFor(entries_[cursorIndex_]);
  const int64_t frameDurationUs =
      (std::max)(int64_t{1}, entries_[cursorIndex_].durationUs);
  const int64_t inverseReserveUs =
      (std::min)(playback_video_frame_step_prefetch::kRefillLeadDurationUs,
                 frameDurationUs <=
                         (std::numeric_limits<int64_t>::max)() /
                             static_cast<int64_t>(
                                 playback_video_frame_step_prefetch::
                                     kDirectionChangeReserveFrameCount)
                     ? frameDurationUs *
                           static_cast<int64_t>(
                               playback_video_frame_step_prefetch::
                                   kDirectionChangeReserveFrameCount)
                     : playback_video_frame_step_prefetch::
                           kRefillLeadDurationUs);
  if (horizonUs == 0) {
    horizonUs = inverseReserveUs;
  }

  // Before planning the active one-second refill, evict outside its protected
  // working set. This is cache eviction by distance from the playhead, not a
  // second coverage model. The smaller inverse request must not reverse that
  // eviction policy.
  if (horizonUs == playback_video_frame_step_prefetch::kWindowDurationUs) {
    if (sourceCache_.find(currentIdentity)) {
      const int64_t keepBefore =
          direction == playback_video_frame_step::Direction::Previous
              ? playback_video_frame_step_prefetch::kWindowDurationUs
              : inverseReserveUs;
      const int64_t keepAfter =
          direction == playback_video_frame_step::Direction::Next
              ? playback_video_frame_step_prefetch::kWindowDurationUs
              : inverseReserveUs;
      sourceCache_.retainWindow(
          currentIdentity, keepBefore, keepAfter,
          playback_video_frame_step_prefetch::kMaxCachedFrameCount);
      refreshFrameStepView(currentIdentity);
    } else if (!sourceCache_.empty()) {
      // A transport seek landed outside the cached component. Old immutable
      // frames are no longer the active working set and must release budget
      // before the new component is decoded.
      sourceCache_.clear();
      sourceStartIdentity_.reset();
      sourceEndIdentity_.reset();
    }
  }

  const PresentedFrame& current = entries_[cursorIndex_];
  currentIdentity = identityFor(current);
  const int64_t currentEndUs = frameEndUs(current);
  if ((direction == playback_video_frame_step::Direction::Previous &&
       current.ptsUs <= 0) ||
      (direction == playback_video_frame_step::Direction::Next &&
       mediaDurationUs > 0 && currentEndUs >= mediaDurationUs)) {
    return std::nullopt;
  }

  auto boundedAdd = [](int64_t value, int64_t extension) {
    return value <= (std::numeric_limits<int64_t>::max)() - extension
               ? value + extension
               : (std::numeric_limits<int64_t>::max)();
  };

  playback_video_frame_step_prefetch::Boundary join = boundaryFor(current);
  bool joinCached = false;
  size_t activeFrameCount = 0;
  int64_t activeDurationUs = 0;
  if (sourceCache_.find(currentIdentity)) {
    playback_video_frame_step_prefetch::FrameWindow cached =
        sourceCache_.windowAround(
            currentIdentity,
            playback_video_frame_step_prefetch::kWindowDurationUs +
                playback_video_frame_step_prefetch::kRefillLeadDurationUs,
            playback_video_frame_step_prefetch::kWindowDurationUs +
                playback_video_frame_step_prefetch::kRefillLeadDurationUs,
            playback_video_frame_step_prefetch::kMaxCachedFrameCount);
    if (!cached.valid()) {
      return std::nullopt;
    }
    const auto& edge =
        direction == playback_video_frame_step::Direction::Previous
            ? cached.frames.front()
            : cached.frames.back();
    join.identity = edge->identity;
    join.ptsUs = edge->ptsUs;
    join.sourcePtsUs = edge->sourcePtsUs;
    join.durationUs = edge->durationUs;
    joinCached = true;
    activeFrameCount =
        direction == playback_video_frame_step::Direction::Previous
            ? cached.anchorIndex
            : cached.frames.size() - cached.anchorIndex - 1;
    activeDurationUs =
        direction == playback_video_frame_step::Direction::Previous
            ? (std::max)(int64_t{0},
                         current.ptsUs - cached.frames.front()->ptsUs)
            : (std::max)(int64_t{0},
                         presentationFrameEndUs(*cached.frames.back()) -
                             currentEndUs);
  }

  if ((direction == playback_video_frame_step::Direction::Previous &&
       sourceStartIdentity_ &&
       playback_video_frame_step_prefetch::sameIdentity(
           join.identity, *sourceStartIdentity_)) ||
      (direction == playback_video_frame_step::Direction::Next &&
       sourceEndIdentity_ &&
       playback_video_frame_step_prefetch::sameIdentity(join.identity,
                                                        *sourceEndIdentity_))) {
    return std::nullopt;
  }

  if (joinCached && !playback_video_frame_step_prefetch::refillNeeded(
                        activeFrameCount, activeDurationUs)) {
    return std::nullopt;
  }

  int64_t rangeStartUs = join.ptsUs;
  int64_t rangeEndUs = frameEndUs(current);
  if (direction == playback_video_frame_step::Direction::Previous) {
    rangeStartUs =
        current.ptsUs > horizonUs ? current.ptsUs - horizonUs : int64_t{0};
    rangeEndUs = boundedAdd(join.ptsUs, join.durationUs);
    if (join.ptsUs <= rangeStartUs) {
      return std::nullopt;
    }
  } else {
    rangeStartUs = join.ptsUs;
    rangeEndUs = boundedAdd(currentEndUs, horizonUs);
    if (mediaDurationUs > 0) {
      rangeEndUs = (std::min)(rangeEndUs, mediaDurationUs);
    }
    const int64_t joinEndUs = boundedAdd(join.ptsUs, join.durationUs);
    if (joinEndUs >= rangeEndUs) {
      return std::nullopt;
    }
  }

  if (rangeEndUs <= rangeStartUs) {
    return std::nullopt;
  }

  playback_video_frame_step_prefetch::Request request;
  request.serial = serial_;
  request.direction = direction;
  request.boundary = boundaryFor(current);
  request.join = join;
  request.rangeStartUs = rangeStartUs;
  request.rangeEndUs = rangeEndUs;
  request.sourceRangeStartUs = rangeStartUs;
  request.sourceRangeEndUs = rangeEndUs;
  request.joinCached = joinCached;
  if (failedPrefetchRequest_ &&
      samePrefetchRequest(*failedPrefetchRequest_, request)) {
    return std::nullopt;
  }
  return request;
}

void Controller::notePrefetchFailure(
    const playback_video_frame_step_prefetch::Request& request) {
  if (frameStepMode_ && request.serial == serial_) {
    failedPrefetchRequest_ = request;
  }
}

void Controller::noteDecoded(const QueuedFrame& item) {
  assert(item.serial > 0);
  assert(item.serial <= static_cast<uint64_t>(std::numeric_limits<int>::max()));
  assert(static_cast<int>(item.serial) == serial_);
  assert(item.durationUs > 0);
  assert(item.displayIndex > 0);

  if (pendingFrameStepSeek_.active() || frameStepMode_) {
    return;
  }
  if (recordByDisplayIndex(item.displayIndex)) {
    return;
  }

  uint64_t recordLogicalIndex = item.displayIndex;
  if (currentLogicalIndex_ > 0) {
    if (playback_video_frame_step_seek::FrameRecord* next =
            mutableRecordByLogicalIndex(currentLogicalIndex_ + 1)) {
      if (next->ptsUs == item.ptsUs) {
        next->durationUs = item.durationUs;
        next->serial = item.serial;
        next->displayIndex = item.displayIndex;
        publishReplayPending(!atNewestFrame());
        return;
      }
    } else if (item.displayIndex <= currentLogicalIndex_) {
      recordLogicalIndex = currentLogicalIndex_ + 1;
    }
  }

  if (playback_video_frame_step_seek::FrameRecord* record =
          mutableRecordByLogicalIndex(recordLogicalIndex)) {
    if (record->ptsUs != item.ptsUs) {
      assert(false && "Decoded frame identity must map to the same PTS");
      std::abort();
    }
    record->durationUs = item.durationUs;
    record->serial = item.serial;
    record->displayIndex = item.displayIndex;
    publishReplayPending(!atNewestFrame());
    return;
  }
  if (!records_.empty() &&
      recordLogicalIndex <= records_.back().logicalIndex) {
    assert(false && "Decoded frame identity must advance monotonically");
    std::abort();
  }

  playback_video_frame_step_seek::FrameRecord record;
  record.ptsUs = item.ptsUs;
  record.durationUs = item.durationUs;
  record.serial = item.serial;
  record.displayIndex = item.displayIndex;
  record.logicalIndex = recordLogicalIndex;
  records_.push_back(record);
  publishReplayPending(!atNewestFrame());
}

void Controller::appendPresented(const QueuedFrame& item,
                                 const VideoFrame& frame) {
  assert(item.serial > 0);
  assert(item.serial <= static_cast<uint64_t>(std::numeric_limits<int>::max()));
  assert(static_cast<int>(item.serial) == serial_);
  assert(item.durationUs > 0);
  assert(item.displayIndex > 0);

  if (frameStepMode_ && !pendingFrameStepSeek_.active()) {
    appendPresentedInFrameStepMode(item, frame);
    return;
  }

  playback_video_frame_step_seek::FrameRecord record =
      recordForPresented(item);

  PresentedFrame entry;
  entry.frame = frame;
  entry.sourceFrame.reset();
  entry.info = item.info;
  entry.ptsUs = item.ptsUs;
  entry.sourcePtsUs = item.sourcePtsUs;
  entry.durationUs = item.durationUs;
  entry.sourceDurationUs = item.sourceDurationUs;
  entry.serial = item.serial;
  entry.displayIndex = item.displayIndex;
  entry.logicalIndex = record.logicalIndex;
  entry.decodeMs = item.decodeMs;

  if (!entries_.empty() && cursorIndex_ + 1 < entries_.size()) {
    std::ptrdiff_t eraseOffset =
        static_cast<std::ptrdiff_t>(cursorIndex_ + 1);
    entries_.erase(entries_.begin() + eraseOffset, entries_.end());
  }
  if (!entries_.empty()) {
    assert(record.logicalIndex > entries_.back().logicalIndex);
  }

  entries_.push_back(std::move(entry));
  while (entries_.size() > kRetainedFrameCount) {
    entries_.pop_front();
  }
  cursorIndex_ = entries_.empty() ? 0 : entries_.size() - 1;
  publishReplayPending(!atNewestFrame());
}

const PresentedFrame* Controller::peekPrevious() const {
  if (entries_.empty() || cursorIndex_ == 0) {
    return nullptr;
  }
  return &entries_[cursorIndex_ - 1];
}

const PresentedFrame* Controller::peekNext() const {
  if (entries_.empty() || cursorIndex_ + 1 >= entries_.size()) {
    return nullptr;
  }
  return &entries_[cursorIndex_ + 1];
}

bool Controller::decodedNextIsStale(const QueuedFrame& item) const {
  if (!frameStepMode_ || entries_.empty() || cursorIndex_ >= entries_.size()) {
    return false;
  }
  const PresentedFrame& current = entries_[cursorIndex_];
  if (item.ptsUs < current.ptsUs) {
    return true;
  }
  if (item.ptsUs > current.ptsUs) {
    return false;
  }
  return playback_video_frame_step_prefetch::sameIdentity(
      playback_video_frame_step_prefetch::identityFrom(item.info, item.ptsUs,
                                                       item.durationUs),
      identityFor(current));
}

StepTarget Controller::target(
    playback_video_frame_step::Direction direction) const {
  StepTarget result;
  if (const PresentedFrame* entry = retainedStepTarget(direction)) {
    result.kind = StepTargetKind::Present;
    result.frame = entry;
    result.seek.direction = direction;
    result.seek.target.ptsUs = entry->ptsUs;
    result.seek.target.durationUs = entry->durationUs;
    result.seek.target.serial = entry->serial;
    result.seek.target.displayIndex = entry->displayIndex;
    result.seek.target.logicalIndex = entry->logicalIndex;
    result.seek.anchor = result.seek.target;
    return result;
  }

  if (currentLogicalIndex_ == 0) {
    return result;
  }
  uint64_t targetLogicalIndex = currentLogicalIndex_;
  if (direction == playback_video_frame_step::Direction::Previous) {
    if (targetLogicalIndex <= 1) {
      if (const playback_video_frame_step_seek::FrameRecord* current =
              recordByLogicalIndex(currentLogicalIndex_)) {
        if (current->ptsUs <= 0) {
          return result;
        }
        result.kind = StepTargetKind::Seek;
        result.seek.mode =
            playback_video_frame_step_seek::PlanMode::PreviousBeforeTarget;
        result.seek.direction = direction;
        result.seek.target = *current;
        result.seek.anchor = discoveryAnchorForPrevious(*current);
      }
      return result;
    }
    --targetLogicalIndex;
  } else {
    ++targetLogicalIndex;
  }

  if (const playback_video_frame_step_seek::FrameRecord* record =
          recordByLogicalIndex(targetLogicalIndex)) {
    result.kind = StepTargetKind::Seek;
    result.seek.direction = direction;
    result.seek.target = *record;
    result.seek.anchor = seekAnchorFor(*record);
  }
  return result;
}

const PresentedFrame* Controller::step(
    playback_video_frame_step::Direction direction) {
  const PresentedFrame* entry = retainedStepTarget(direction);
  if (!entry) {
    publishReplayPending(!atNewestFrame());
    return nullptr;
  }
  if (direction == playback_video_frame_step::Direction::Previous) {
    --cursorIndex_;
  } else {
    ++cursorIndex_;
  }
  currentLogicalIndex_ = entries_[cursorIndex_].logicalIndex;
  publishReplayPending(!atNewestFrame());
  return &entries_[cursorIndex_];
}

PendingSeekFrameDecision Controller::inspectPendingSeekFrame(
    const QueuedFrame& item, const VideoFrame* frame) {
  PendingSeekFrameDecision decision;
  if (!pendingFrameStepSeek_.active()) {
    return decision;
  }
  if (pendingFrameStepSeek_.mode ==
      playback_video_frame_step_seek::PlanMode::PreviousBeforeTarget) {
    return inspectPreviousDiscoveryFrame(item, frame);
  }
  return inspectExactPendingSeekFrame(item);
}

PendingSeekFrameDecision Controller::inspectExactPendingSeekFrame(
    const QueuedFrame& item) {
  PendingSeekFrameDecision decision;
  const playback_video_frame_step_seek::FrameRecord& target =
      *pendingFrameStepSeek_.target;
  decision.target = target;
  assert(target.valid());
  assert(pendingFrameStepSeek_.prerollLogicalIndex > 0);

  if (pendingFrameStepSeek_.prerollLogicalIndex < target.logicalIndex) {
    const playback_video_frame_step_seek::FrameRecord* expected =
        recordByLogicalIndex(pendingFrameStepSeek_.prerollLogicalIndex);
    assert(expected);
    if (expected && expected->ptsUs == item.ptsUs) {
      if (playback_video_frame_step_seek::FrameRecord* mutableExpected =
              mutableRecordByLogicalIndex(expected->logicalIndex)) {
        mutableExpected->durationUs = item.durationUs;
        mutableExpected->serial = item.serial;
        mutableExpected->displayIndex = item.displayIndex;
      }
      ++pendingFrameStepSeek_.prerollLogicalIndex;
      decision.action = PendingSeekFrameAction::DropPreroll;
      return decision;
    }
    if (item.ptsUs < target.ptsUs) {
      decision.action = PendingSeekFrameAction::DropPreroll;
      return decision;
    }
  }

  if (item.ptsUs == target.ptsUs) {
    if (playback_video_frame_step_seek::FrameRecord* mutableTarget =
            mutableRecordByLogicalIndex(target.logicalIndex)) {
      mutableTarget->durationUs = item.durationUs;
      mutableTarget->serial = item.serial;
      mutableTarget->displayIndex = item.displayIndex;
    }
    pendingFrameStepSeek_.prerollLogicalIndex = target.logicalIndex;
    pendingFrameStepSeek_.targetReady = true;
    decision.action = PendingSeekFrameAction::PresentTarget;
    return decision;
  }

  if (item.ptsUs < target.ptsUs) {
    decision.action = PendingSeekFrameAction::DropPreroll;
    return decision;
  }

  decision.action = PendingSeekFrameAction::MissedTarget;
  return decision;
}

bool Controller::atNewestFrame() const {
  bool retainedAtNewest = entries_.empty() || cursorIndex_ + 1 == entries_.size();
  bool logicalAtNewest =
      records_.empty() || currentLogicalIndex_ == records_.back().logicalIndex;
  return retainedAtNewest && logicalAtNewest;
}

bool Controller::frameStepSeekPendingForSerial(int serial) const {
  return pendingFrameStepSeek_.active() && serial_ == serial;
}

bool Controller::cancelPendingFrameStepSeekForSerial(int serial) {
  if (!pendingFrameStepSeek_.active() || serial_ != serial) {
    return false;
  }
  pendingFrameStepSeek_.clear();
  publishReplayPending(false);
  return true;
}

bool Controller::exitFrameStepModeForPlaybackResume(int serial) {
  if (serial_ != serial ||
      (!frameStepMode_ && !pendingFrameStepSeek_.active())) {
    return false;
  }
  pendingFrameStepSeek_.clear();
  entries_.clear();
  records_.clear();
  cursorIndex_ = 0;
  frameStepMode_ = false;
  currentLogicalIndex_ = 0;
  failedPrefetchRequest_.reset();
  publishReplayPending(false);
  return true;
}

std::optional<playback_video_frame_step::Request>
Controller::pendingFrameStepRequestForSerial(int serial) const {
  if (!pendingFrameStepSeek_.targetReady || !pendingFrameStepSeek_.active() ||
      serial_ != serial || pendingFrameStepSeek_.generation == 0) {
    return std::nullopt;
  }
  playback_video_frame_step::Request request;
  request.direction = pendingFrameStepSeek_.direction;
  request.serial = serial;
  request.generation = pendingFrameStepSeek_.generation;
  return request;
}

bool Controller::replayPendingForSerial(int serial) const {
  if (!replayPending_.load(std::memory_order_acquire)) {
    return false;
  }
  return replaySerial_.load(std::memory_order_relaxed) == serial;
}

void Controller::publishReplayPending(bool pending) {
  replaySerial_.store(serial_, std::memory_order_relaxed);
  replayPending_.store(pending, std::memory_order_release);
}

const PresentedFrame* Controller::retainedStepTarget(
    playback_video_frame_step::Direction direction) const {
  const PresentedFrame* candidate =
      direction == playback_video_frame_step::Direction::Previous
          ? peekPrevious()
          : peekNext();
  if (!candidate || currentLogicalIndex_ == 0) {
    return nullptr;
  }
  uint64_t expectedLogicalIndex =
      direction == playback_video_frame_step::Direction::Previous
          ? currentLogicalIndex_ - 1
          : currentLogicalIndex_ + 1;
  if (expectedLogicalIndex == 0 ||
      candidate->logicalIndex != expectedLogicalIndex) {
    return nullptr;
  }
  return candidate;
}

const playback_video_frame_step_seek::FrameRecord* Controller::recordByLogicalIndex(
    uint64_t logicalIndex) const {
  if (records_.empty() || logicalIndex == 0) {
    return nullptr;
  }

  const uint64_t first = records_.front().logicalIndex;
  if (logicalIndex >= first) {
    uint64_t offset = logicalIndex - first;
    if (offset < records_.size()) {
      const playback_video_frame_step_seek::FrameRecord& record =
          records_[static_cast<size_t>(offset)];
      if (record.logicalIndex == logicalIndex) {
        return &record;
      }
    }
  }

  auto it = std::find_if(records_.begin(), records_.end(),
                         [logicalIndex](
                             const playback_video_frame_step_seek::FrameRecord&
                                 record) {
                           return record.logicalIndex == logicalIndex;
                         });
  return it == records_.end() ? nullptr : &*it;
}

const playback_video_frame_step_seek::FrameRecord* Controller::recordByDisplayIndex(
    uint64_t displayIndex) const {
  if (displayIndex == 0) {
    return nullptr;
  }
  auto it = std::find_if(records_.begin(), records_.end(),
                         [&](const playback_video_frame_step_seek::FrameRecord&
                                 record) {
                           return record.serial ==
                                      static_cast<uint64_t>(serial_) &&
                                  record.displayIndex == displayIndex;
                         });
  return it == records_.end() ? nullptr : &*it;
}

playback_video_frame_step_seek::FrameRecord*
Controller::mutableRecordByLogicalIndex(uint64_t logicalIndex) {
  if (records_.empty() || logicalIndex == 0) {
    return nullptr;
  }

  const uint64_t first = records_.front().logicalIndex;
  if (logicalIndex >= first) {
    uint64_t offset = logicalIndex - first;
    if (offset < records_.size()) {
      playback_video_frame_step_seek::FrameRecord& record =
          records_[static_cast<size_t>(offset)];
      if (record.logicalIndex == logicalIndex) {
        return &record;
      }
    }
  }

  auto it = std::find_if(records_.begin(), records_.end(),
                         [logicalIndex](
                             const playback_video_frame_step_seek::FrameRecord&
                                 record) {
                           return record.logicalIndex == logicalIndex;
                         });
  return it == records_.end() ? nullptr : &*it;
}

playback_video_frame_step_seek::FrameRecord Controller::seekAnchorFor(
    const playback_video_frame_step_seek::FrameRecord& target) const {
  playback_video_frame_step_seek::FrameRecord anchor = target;
  if (target.logicalIndex > 1) {
    if (const playback_video_frame_step_seek::FrameRecord* previous =
            recordByLogicalIndex(target.logicalIndex - 1)) {
      anchor = *previous;
    }
  }
  while (anchor.logicalIndex > 1) {
    const playback_video_frame_step_seek::FrameRecord* previous =
        recordByLogicalIndex(anchor.logicalIndex - 1);
    if (!previous || previous->ptsUs != anchor.ptsUs) {
      break;
    }
    anchor = *previous;
  }
  return anchor;
}

playback_video_frame_step_seek::FrameRecord
Controller::discoveryAnchorForPrevious(
    const playback_video_frame_step_seek::FrameRecord& boundary) const {
  return boundary;
}

PendingSeekFrameDecision Controller::inspectPreviousDiscoveryFrame(
    const QueuedFrame& item, const VideoFrame* frame) {
  PendingSeekFrameDecision decision;
  const playback_video_frame_step_seek::FrameRecord& boundary =
      *pendingFrameStepSeek_.target;
  decision.target = boundary;
  assert(boundary.valid());

  if (item.ptsUs < boundary.ptsUs) {
    assert(frame);
    if (!frame) {
      std::abort();
    }
    PresentedFrame candidate;
    candidate.frame = *frame;
    candidate.info = item.info;
    candidate.ptsUs = item.ptsUs;
    candidate.sourcePtsUs = item.sourcePtsUs;
    candidate.durationUs = item.durationUs;
    candidate.sourceDurationUs = item.sourceDurationUs;
    candidate.serial = item.serial;
    candidate.displayIndex = item.displayIndex;
    candidate.logicalIndex = boundary.logicalIndex > 1
                                 ? boundary.logicalIndex - 1
                                 : uint64_t{1};
    candidate.decodeMs = item.decodeMs;
    pendingFrameStepSeek_.backstepCandidate = candidate;
    decision.target = playback_video_frame_step_seek::FrameRecord{
        candidate.ptsUs, candidate.durationUs, candidate.serial,
        candidate.displayIndex, candidate.logicalIndex};
    decision.frame = &*pendingFrameStepSeek_.backstepCandidate;
    decision.action = PendingSeekFrameAction::SaveBackstepCandidate;
    return decision;
  }

  if (!pendingFrameStepSeek_.backstepCandidate) {
    decision.action = PendingSeekFrameAction::CancelWithoutDropping;
    return decision;
  }

  if (boundary.logicalIndex <= 1) {
    shiftKnownRecordsForward();
  }

  PresentedFrame& candidate = *pendingFrameStepSeek_.backstepCandidate;
  candidate.logicalIndex = pendingFrameStepSeek_.target->logicalIndex > 1
                               ? pendingFrameStepSeek_.target->logicalIndex - 1
                               : uint64_t{1};
  playback_video_frame_step_seek::FrameRecord previous;
  previous.ptsUs = candidate.ptsUs;
  previous.durationUs = candidate.durationUs;
  previous.serial = candidate.serial;
  previous.displayIndex = candidate.displayIndex;
  previous.logicalIndex = candidate.logicalIndex;
  records_.insert(records_.begin(), previous);

  pendingFrameStepSeek_.target = previous;
  pendingFrameStepSeek_.prerollLogicalIndex = previous.logicalIndex;
  pendingFrameStepSeek_.targetReady = true;
  decision.target = previous;
  decision.frame = &candidate;
  decision.action = PendingSeekFrameAction::PresentBackstepCandidate;
  return decision;
}

void Controller::shiftKnownRecordsForward() {
  for (playback_video_frame_step_seek::FrameRecord& record : records_) {
    ++record.logicalIndex;
  }
  for (PresentedFrame& entry : entries_) {
    ++entry.logicalIndex;
  }
  if (currentLogicalIndex_ > 0) {
    ++currentLogicalIndex_;
  }
  if (pendingFrameStepSeek_.target) {
    ++pendingFrameStepSeek_.target->logicalIndex;
  }
  if (pendingFrameStepSeek_.prerollLogicalIndex > 0) {
    ++pendingFrameStepSeek_.prerollLogicalIndex;
  }
}

playback_video_frame_step_prefetch::Boundary Controller::boundaryFor(
    const PresentedFrame& frame) {
  playback_video_frame_step_prefetch::Boundary boundary;
  boundary.identity = identityFor(frame);
  boundary.ptsUs = frame.ptsUs;
  boundary.durationUs = frame.durationUs;
  boundary.sourcePtsUs = frame.sourcePtsUs;
  return boundary;
}

playback_video_frame_step_prefetch::FrameIdentity Controller::identityFor(
    const PresentedFrame& frame) {
  if (frame.sourceFrame) {
    return frame.sourceFrame->identity;
  }
  return playback_video_frame_step_prefetch::identityFrom(
      frame.info, frame.sourcePtsUs, frame.sourceDurationUs);
}

std::optional<size_t> Controller::entryIndexForIdentity(
    const playback_video_frame_step_prefetch::FrameIdentity& identity) const {
  for (size_t index = 0; index < entries_.size(); ++index) {
    if (playback_video_frame_step_prefetch::sameIdentity(
            identityFor(entries_[index]), identity)) {
      return index;
    }
  }
  return std::nullopt;
}

bool Controller::refreshFrameStepView(
    const playback_video_frame_step_prefetch::FrameIdentity& currentIdentity) {
  playback_video_frame_step_prefetch::FrameWindow cached =
      sourceCache_.windowAround(
          currentIdentity,
          playback_video_frame_step_prefetch::kWindowDurationUs +
              playback_video_frame_step_prefetch::kRefillLeadDurationUs,
          playback_video_frame_step_prefetch::kWindowDurationUs +
              playback_video_frame_step_prefetch::kRefillLeadDurationUs,
          playback_video_frame_step_prefetch::kMaxCachedFrameCount);
  if (!cached.valid()) {
    return false;
  }

  std::deque<PresentedFrame> refreshed;
  for (const auto& source : cached.frames) {
    if (!source || source->ptsUs < 0 || source->sourcePtsUs < 0 ||
        source->durationUs <= 0 || source->sourceDurationUs <= 0) {
      return false;
    }
    PresentedFrame entry;
    entry.sourceFrame = source;
    entry.info = source->info;
    entry.ptsUs = source->ptsUs;
    entry.sourcePtsUs = source->sourcePtsUs;
    entry.durationUs = source->durationUs;
    entry.sourceDurationUs = source->sourceDurationUs;
    entry.serial = static_cast<uint64_t>(serial_);
    entry.decodeMs = source->decodeMs;
    refreshed.push_back(std::move(entry));
  }

  entries_.swap(refreshed);
  rebuildFrameStepRecords(currentIdentity);
  return true;
}

void Controller::appendPresentedInFrameStepMode(const QueuedFrame& item,
                                                const VideoFrame& frame) {
  const playback_video_frame_step_prefetch::FrameIdentity identity =
      playback_video_frame_step_prefetch::identityFrom(
          item.info, item.sourcePtsUs, item.sourceDurationUs);
  if (std::optional<size_t> existing = entryIndexForIdentity(identity)) {
    PresentedFrame& entry = entries_[*existing];
    entry.frame = frame;
    entry.sourceFrame.reset();
    entry.info = item.info;
    entry.ptsUs = item.ptsUs;
    entry.sourcePtsUs = item.sourcePtsUs;
    entry.durationUs = item.durationUs;
    entry.sourceDurationUs = item.sourceDurationUs;
    entry.serial = item.serial;
    entry.decodeMs = item.decodeMs;
  } else {
    if (!entries_.empty() && cursorIndex_ + 1 < entries_.size()) {
      entries_.erase(
          entries_.begin() + static_cast<std::ptrdiff_t>(cursorIndex_ + 1),
          entries_.end());
    }
    PresentedFrame entry;
    entry.frame = frame;
    entry.sourceFrame.reset();
    entry.info = item.info;
    entry.ptsUs = item.ptsUs;
    entry.sourcePtsUs = item.sourcePtsUs;
    entry.durationUs = item.durationUs;
    entry.sourceDurationUs = item.sourceDurationUs;
    entry.serial = item.serial;
    entry.decodeMs = item.decodeMs;
    entries_.push_back(std::move(entry));
  }

  failedPrefetchRequest_.reset();
  trimFrameStepWindow(identity);
  rebuildFrameStepRecords(identity);
  publishReplayPending(!atNewestFrame());
}

void Controller::trimFrameStepWindow(
    const playback_video_frame_step_prefetch::FrameIdentity& currentIdentity) {
  while (entries_.size() > 1) {
    const std::optional<size_t> currentIndex =
        entryIndexForIdentity(currentIdentity);
    if (!currentIndex) {
      assert(false && "Frame-step cache must retain its current frame");
      std::abort();
    }
    const int64_t windowDurationUs =
        (std::max)(int64_t{0},
                   frameEndUs(entries_.back()) - entries_.front().ptsUs);
    const size_t decoderBackedFrameCount = static_cast<size_t>(std::count_if(
        entries_.begin(), entries_.end(), [](const PresentedFrame& entry) {
          return static_cast<bool>(entry.videoFrame().hwFrameRef);
        }));
    if (entries_.size() <=
            playback_video_frame_step_prefetch::kMaxCachedFrameCount &&
        windowDurationUs <=
            playback_video_frame_step_prefetch::kWindowDurationUs &&
        decoderBackedFrameCount <= kRetainedFrameCount) {
      break;
    }

    const int64_t beforeDistanceUs =
        (std::max)(int64_t{0},
                   entries_[*currentIndex].ptsUs - entries_.front().ptsUs);
    const int64_t afterDistanceUs =
        (std::max)(int64_t{0}, frameEndUs(entries_.back()) -
                                   frameEndUs(entries_[*currentIndex]));
    if (*currentIndex > 0 && (*currentIndex + 1 == entries_.size() ||
                              beforeDistanceUs >= afterDistanceUs)) {
      entries_.pop_front();
    } else {
      entries_.pop_back();
    }
  }
}

void Controller::rebuildFrameStepRecords(
    const playback_video_frame_step_prefetch::FrameIdentity& currentIdentity) {
  const std::optional<size_t> currentIndex =
      entryIndexForIdentity(currentIdentity);
  if (!currentIndex) {
    assert(false && "Frame-step cache must contain its current frame");
    std::abort();
  }

  records_.clear();
  records_.reserve(entries_.size());
  uint64_t logicalIndex = 1;
  for (PresentedFrame& entry : entries_) {
    entry.serial = static_cast<uint64_t>(serial_);
    entry.displayIndex = logicalIndex;
    entry.logicalIndex = logicalIndex;
    playback_video_frame_step_seek::FrameRecord record;
    record.ptsUs = entry.ptsUs;
    record.durationUs = entry.durationUs;
    record.serial = entry.serial;
    record.displayIndex = entry.displayIndex;
    record.logicalIndex = entry.logicalIndex;
    records_.push_back(record);
    ++logicalIndex;
  }
  cursorIndex_ = *currentIndex;
  currentLogicalIndex_ = entries_[cursorIndex_].logicalIndex;
}

playback_video_frame_step_seek::FrameRecord Controller::recordForPresented(
    const QueuedFrame& item) {
  if (pendingFrameStepSeek_.active()) {
    assert(pendingFrameStepSeek_.targetReady);
    assert(item.ptsUs == pendingFrameStepSeek_.target->ptsUs);
    playback_video_frame_step_seek::FrameRecord record =
        *pendingFrameStepSeek_.target;
    pendingFrameStepSeek_.clear();
    currentLogicalIndex_ = record.logicalIndex;
    return record;
  }

  if (const playback_video_frame_step_seek::FrameRecord* record =
          recordByDisplayIndex(item.displayIndex)) {
    currentLogicalIndex_ = record->logicalIndex;
    return *record;
  }

  assert(false && "Presented frames must already have decoded identity");
  std::abort();
}

}  // namespace playback_video_frame_cursor
