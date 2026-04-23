#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include "playback/control/command.h"

class PlaybackSystemControls {
 public:
  enum class Status : uint8_t {
    Closed,
    Stopped,
    Playing,
    Paused,
  };

  struct State {
    bool active = false;
    bool isVideo = false;
    std::filesystem::path file;
    int trackIndex = -1;
    Status status = Status::Closed;
    double positionSec = 0.0;
    double durationSec = -1.0;
    bool canPlay = true;
    bool canPause = true;
    bool canStop = true;
    bool canPrevious = false;
    bool canNext = false;
  };

  PlaybackSystemControls();
  ~PlaybackSystemControls();

  PlaybackSystemControls(PlaybackSystemControls&&) noexcept;
  PlaybackSystemControls& operator=(PlaybackSystemControls&&) noexcept;

  PlaybackSystemControls(const PlaybackSystemControls&) = delete;
  PlaybackSystemControls& operator=(const PlaybackSystemControls&) = delete;

  bool initialize();
  bool available() const;
  void clear();
  void update(const State& state);
  bool pollCommand(PlaybackControlCommand* out);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
