#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "core/native_wait_handle.h"
#include "core/wake_deadline.h"
#include "playback/session/open_outcome.h"
#include "playback/session/opening_backend.h"
#include "playback/session/transition_state.h"

struct InputEvent;

class PlaybackSessionBootstrap {
 public:
  using Now = wake_schedule::TimePoint (*)();

  struct Args {
    const std::filesystem::path& file;
    bool enableAudio;
    bool enableAscii;
    playback_session::OpeningBackend& backend;
    Now now = nullptr;
  };

  explicit PlaybackSessionBootstrap(Args args);
  ~PlaybackSessionBootstrap();

  PlaybackSessionBootstrap(const PlaybackSessionBootstrap&) = delete;
  PlaybackSessionBootstrap& operator=(const PlaybackSessionBootstrap&) = delete;

  PlaybackSessionBootstrap(PlaybackSessionBootstrap&&) noexcept;
  PlaybackSessionBootstrap& operator=(PlaybackSessionBootstrap&&) noexcept;

  // Starts decoder initialization without taking over the application event
  // loop. An empty result means initialization remains in progress; the owner
  // must then include waitHandles()/nextWakeDeadline(), route input through
  // handleInputEvent(), and call pump() until it produces a terminal outcome.
  std::optional<playback_session::OpenOutcome> start();
  std::optional<playback_session::OpenOutcome> pump();
  bool handleInputEvent(const InputEvent& event);
  playback_session::TransitionSnapshot snapshot() const;
  std::vector<NativeWaitHandle> waitHandles() const;
  wake_schedule::Deadline nextWakeDeadline() const;
  void requestCancel();
  void requestQuit();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
