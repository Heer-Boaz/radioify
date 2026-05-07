#include "frame_cursor.h"

#include <cassert>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

#include "queues.h"

namespace playback_video_frame_cursor {

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
  if (seekPlan) {
    pendingFrameStepSeek_.begin(*seekPlan);
    publishReplayPending(true);
    return;
  }

  pendingFrameStepSeek_.clear();
  records_.clear();
  currentLogicalIndex_ = 0;
  publishReplayPending(false);
}

void Controller::noteDecoded(const QueuedFrame& item) {
  assert(item.serial > 0);
  assert(item.serial <= static_cast<uint64_t>(std::numeric_limits<int>::max()));
  assert(static_cast<int>(item.serial) == serial_);
  assert(item.durationUs > 0);
  assert(item.displayIndex > 0);

  if (pendingFrameStepSeek_.active()) {
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

  playback_video_frame_step_seek::FrameRecord record =
      recordForPresented(item);

  PresentedFrame entry;
  entry.frame = frame;
  entry.info = item.info;
  entry.ptsUs = item.ptsUs;
  entry.durationUs = item.durationUs;
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
    candidate.durationUs = item.durationUs;
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
