#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "core/native_wait_handle.h"
#include "playback/video/decoder.h"
#include "playback/video/frame_step.h"
#include "playback/video/state/types.h"
#include "playback/video/timing/clock_source.h"

struct PlayerConfig {
  std::filesystem::path file;
  std::filesystem::path logPath;
  bool enableAudio = true;
  bool allowDecoderScale = true;
  int targetWidth = 0;
  int targetHeight = 0;
};

struct PlayerDebugInfo {
  PlayerState state = PlayerState::Idle;
  PlayerClockSource masterSource = PlayerClockSource::None;
  int currentSerial = 0;
  int pendingSeekSerial = 0;
  int seekInFlightSerial = 0;
  bool audioOk = false;
  bool audioStarved = false;
  bool audioClockReady = false;
  bool audioClockFresh = false;
  int64_t audioClockUs = 0;
  int64_t audioClockUpdatedUs = 0;
  int64_t masterClockUs = 0;
  int64_t lastDiffUs = 0;
  int64_t lastDelayUs = 0;
  int64_t lastPresentedPtsUs = 0;
  int64_t lastPresentedDurationUs = 0;
  uint64_t lastPresentedDisplayIndex = 0;
  size_t videoQueueDepth = 0;
  bool hasVideoFrame = false;
  int videoFrameWidth = 0;
  int videoFrameHeight = 0;
  VideoPixelFormat videoFrameFormat = VideoPixelFormat::Unknown;
  YuvMatrix videoFrameMatrix = YuvMatrix::Bt709;
  YuvTransfer videoFrameTransfer = YuvTransfer::Sdr;
  bool videoFrameFullRange = true;
  size_t audioBufferedFrames = 0;
  uint32_t audioSampleRate = 0;
};

class Player {
 public:
  Player();
  ~Player();

  bool open(const PlayerConfig& config, std::string* error);
  void close();

  void requestSeek(int64_t targetUs);
  bool requestFrameStep(playback_video_frame_step::Direction direction);
  void requestResize(int targetW, int targetH);
  void setVideoPaused(bool paused);
  size_t audioTrackCount() const;
  std::string activeAudioTrackLabel() const;
  bool canCycleAudioTracks() const;
  bool cycleAudioTrack();

  bool initDone() const;
  bool initOk() const;
  std::string initError() const;

  bool audioStarting() const;
  bool audioOk() const;
  bool audioFinished() const;

  bool seekPending() const;
  bool isSeeking() const;
  bool isBuffering() const;
  bool isEnded() const;
  PlayerState state() const;
  PlayerDebugInfo debugInfo() const;

  int64_t durationUs() const;
  int64_t currentUs() const;

  uint64_t videoFrameCounter() const;
  bool waitForVideoFrame(uint64_t lastCounter, int timeoutMs) const;
  NativeWaitHandle videoFrameWaitHandle() const;
  NativeWaitHandle statusChangeWaitHandle() const;

  bool copyCurrentVideoFrame(VideoFrame* out);
  bool hasVideoFrame() const;
  int sourceWidth() const;
  int sourceHeight() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
