#pragma once

#include <memory>

#include "core/native_wait_handle.h"
#include "playback/control/command.h"
#include "playback/control/system_control_state.h"

class PlaybackSystemControls {
 public:
  using Status = PlaybackControlStatus;
  using State = PlaybackControlState;

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
  bool pollCommand(PlaybackControlCommandEvent* out);
  NativeWaitHandle nativeWaitHandle() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
