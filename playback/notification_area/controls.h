#pragma once

#include <memory>

#include "core/native_wait_handle.h"
#include "playback/control/command.h"
#include "playback/control/system_control_state.h"

struct PlaybackNotificationAreaCommand {
  enum class Kind {
    Activate,
    Playback,
    Quit,
  };

  Kind kind = Kind::Activate;
  PlaybackControlCommand playbackCommand = PlaybackControlCommand::TogglePause;
};

class PlaybackNotificationAreaControls {
 public:
  PlaybackNotificationAreaControls();
  ~PlaybackNotificationAreaControls();

  PlaybackNotificationAreaControls(PlaybackNotificationAreaControls&&) noexcept;
  PlaybackNotificationAreaControls& operator=(
      PlaybackNotificationAreaControls&&) noexcept;

  PlaybackNotificationAreaControls(const PlaybackNotificationAreaControls&) =
      delete;
  PlaybackNotificationAreaControls& operator=(
      const PlaybackNotificationAreaControls&) = delete;

  bool initialize();
  bool available() const;
  void clear();
  void update(const PlaybackControlState& state);
  bool pollCommand(PlaybackNotificationAreaCommand* out);
  NativeWaitHandle nativeWaitHandle() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
