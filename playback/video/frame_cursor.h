#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "playback/video/decoder.h"
#include "playback/video/frame_step.h"
#include "playback/video/frame_step_seek_plan.h"

struct QueuedFrame;

namespace playback_video_frame_cursor {

inline constexpr size_t kRetainedFrameCount = 2;

struct PresentedFrame {
  VideoFrame frame;
  VideoReadInfo info{};
  int64_t ptsUs = 0;
  int64_t durationUs = 0;
  uint64_t serial = 0;
  uint64_t displayIndex = 0;
  uint64_t logicalIndex = 0;
  double decodeMs = 0.0;
};

enum class StepTargetKind {
  None,
  Present,
  Seek,
};

struct StepTarget {
  StepTargetKind kind = StepTargetKind::None;
  const PresentedFrame* frame = nullptr;
  playback_video_frame_step_seek::Plan seek;
};

enum class PendingSeekFrameAction {
  None,
  SaveBackstepCandidate,
  PresentBackstepCandidate,
  DropPreroll,
  PresentTarget,
  CancelWithoutDropping,
  MissedTarget,
};

struct PendingSeekFrameDecision {
  PendingSeekFrameAction action = PendingSeekFrameAction::None;
  playback_video_frame_step_seek::FrameRecord target;
  const PresentedFrame* frame = nullptr;
};

class Controller {
 public:
  void resetForSerial(int serial,
                      const playback_video_frame_step_seek::Plan* seekPlan =
                          nullptr);
  void noteDecoded(const QueuedFrame& item);
  void appendPresented(const QueuedFrame& item, const VideoFrame& frame);
  const PresentedFrame* peekNext() const;
  StepTarget target(playback_video_frame_step::Direction direction) const;
  const PresentedFrame* step(playback_video_frame_step::Direction direction);
  PendingSeekFrameDecision inspectPendingSeekFrame(
      const QueuedFrame& item, const VideoFrame* frame = nullptr);
  bool atNewestFrame() const;
  bool frameStepSeekPendingForSerial(int serial) const;
  bool cancelPendingFrameStepSeekForSerial(int serial);
  bool exitFrameStepModeForPlaybackResume(int serial);
  std::optional<playback_video_frame_step::Request>
  pendingFrameStepRequestForSerial(int serial) const;
  bool replayPendingForSerial(int serial) const;

 private:
  struct PendingFrameStepSeek {
    std::optional<playback_video_frame_step_seek::FrameRecord> target;
    playback_video_frame_step_seek::PlanMode mode =
        playback_video_frame_step_seek::PlanMode::ExactFrame;
    uint64_t prerollLogicalIndex = 0;
    uint64_t generation = 0;
    playback_video_frame_step::Direction direction =
        playback_video_frame_step::Direction::Next;
    bool targetReady = false;
    std::optional<PresentedFrame> backstepCandidate;

    bool active() const { return target.has_value(); }
    void begin(const playback_video_frame_step_seek::Plan& plan);
    void clear();
  };

  const PresentedFrame* peekPrevious() const;
  const PresentedFrame* retainedStepTarget(
      playback_video_frame_step::Direction direction) const;
  const playback_video_frame_step_seek::FrameRecord* recordByLogicalIndex(
      uint64_t logicalIndex) const;
  const playback_video_frame_step_seek::FrameRecord* recordByDisplayIndex(
      uint64_t displayIndex) const;
  playback_video_frame_step_seek::FrameRecord* mutableRecordByLogicalIndex(
      uint64_t logicalIndex);
  playback_video_frame_step_seek::FrameRecord seekAnchorFor(
      const playback_video_frame_step_seek::FrameRecord& target) const;
  playback_video_frame_step_seek::FrameRecord discoveryAnchorForPrevious(
      const playback_video_frame_step_seek::FrameRecord& boundary) const;
  PendingSeekFrameDecision inspectExactPendingSeekFrame(
      const QueuedFrame& item);
  PendingSeekFrameDecision inspectPreviousDiscoveryFrame(
      const QueuedFrame& item, const VideoFrame* frame);
  void shiftKnownRecordsForward();
  playback_video_frame_step_seek::FrameRecord recordForPresented(
      const QueuedFrame& item);
  void publishReplayPending(bool pending);

  std::deque<PresentedFrame> entries_;
  std::vector<playback_video_frame_step_seek::FrameRecord> records_;
  PendingFrameStepSeek pendingFrameStepSeek_;
  std::atomic<int> replaySerial_{0};
  std::atomic<bool> replayPending_{false};
  size_t cursorIndex_ = 0;
  uint64_t currentLogicalIndex_ = 0;
  int serial_ = 0;
};

}  // namespace playback_video_frame_cursor
