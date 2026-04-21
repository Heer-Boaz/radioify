#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "videodecoder.h"

struct PlayerConfig {
  std::filesystem::path file;
  std::filesystem::path logPath;
  bool enableAudio = true;
  bool allowDecoderScale = true;
  int targetWidth = 0;
  int targetHeight = 0;
};

enum class PlayerState {
  Idle,
  Opening,
  Prefill,
  Priming,
  Playing,
  Paused,
  Seeking,
  Draining,
  Ended,
  Error,
  Closing,
};

enum class PlayerClockSource {
  None,
  Audio,
  Video,
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
  size_t videoQueueDepth = 0;
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

  bool isSeeking() const;
  bool isBuffering() const;
  bool isEnded() const;
  PlayerState state() const;
  PlayerDebugInfo debugInfo() const;

  int64_t durationUs() const;
  int64_t currentUs() const;

  uint64_t videoFrameCounter() const;
  bool waitForVideoFrame(uint64_t lastCounter, int timeoutMs) const;
  HANDLE videoFrameWaitHandle() const;

  bool copyCurrentVideoFrame(VideoFrame* out);
  bool hasVideoFrame() const;
  int sourceWidth() const;
  int sourceHeight() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
