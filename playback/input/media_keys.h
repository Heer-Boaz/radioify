#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "playback/control/command.h"

// Synthetic virtual keys for the two APPCOMMANDs Windows exposes without a
// matching VK_MEDIA_* code. They never appear in real keyboard input.
constexpr WORD kPlaybackVkMediaPlay = 0x1000;
constexpr WORD kPlaybackVkMediaPause = 0x1001;

inline constexpr bool isSystemMediaVirtualKey(WORD key) {
  switch (key) {
    case VK_MEDIA_PLAY_PAUSE:
    case VK_MEDIA_STOP:
    case VK_MEDIA_PREV_TRACK:
    case VK_MEDIA_NEXT_TRACK:
      return true;
    default:
      return false;
  }
}

// One physical media-key press is one command, whichever transport reports it.
// Windows resolves the single play/pause key to Play, Pause or PlayPause
// depending on the state it believes the session is in, so the two ingresses
// disagree about the exact command while describing the same press. Grouping
// keeps duplicate detection independent of that disagreement.
enum class SystemMediaCommandGroup : std::uint8_t {
  PlayPause,
  Stop,
  Previous,
  Next,
};

inline constexpr std::size_t kSystemMediaCommandGroupCount = 4;

inline constexpr std::size_t systemMediaCommandGroupIndex(
    SystemMediaCommandGroup group) {
  return static_cast<std::size_t>(group);
}

inline constexpr SystemMediaCommandGroup systemMediaCommandGroup(
    PlaybackControlCommand command) {
  switch (command) {
    case PlaybackControlCommand::Stop:
      return SystemMediaCommandGroup::Stop;
    case PlaybackControlCommand::Previous:
      return SystemMediaCommandGroup::Previous;
    case PlaybackControlCommand::Next:
      return SystemMediaCommandGroup::Next;
    case PlaybackControlCommand::Play:
    case PlaybackControlCommand::Pause:
    case PlaybackControlCommand::TogglePause:
    default:
      return SystemMediaCommandGroup::PlayPause;
  }
}

// Reports nothing for keys that are not media transport keys, so callers can
// route ordinary keyboard input without consulting the arbiter.
inline constexpr bool systemMediaCommandGroupForVirtualKey(
    WORD key, SystemMediaCommandGroup* out) {
  switch (key) {
    case VK_MEDIA_PLAY_PAUSE:
    case kPlaybackVkMediaPlay:
    case kPlaybackVkMediaPause:
      if (out) *out = SystemMediaCommandGroup::PlayPause;
      return true;
    case VK_MEDIA_STOP:
      if (out) *out = SystemMediaCommandGroup::Stop;
      return true;
    case VK_MEDIA_PREV_TRACK:
      if (out) *out = SystemMediaCommandGroup::Previous;
      return true;
    case VK_MEDIA_NEXT_TRACK:
      if (out) *out = SystemMediaCommandGroup::Next;
      return true;
    default:
      return false;
  }
}

// Which transport observed a media command.
enum class SystemMediaCommandIngress : std::uint8_t {
  // WM_APPCOMMAND or a console key record delivered to whichever Radioify
  // surface holds keyboard focus.
  FocusedSurface,
  // The process-wide System Media Transport Controls button callback.
  SystemSession,
};

// Windows can report one media-key press through both ingresses at once, and
// neither can be switched off in advance: whether the press reaches Radioify's
// SMTC session depends on which media session the shell currently treats as
// current, which other players take over without telling us. Disabling the
// focused-surface ingress because an SMTC session exists therefore drops the
// press whenever the shell hands it to someone else. So both ingresses stay
// live and this arbiter admits the first one to report a press, dropping the
// other transport's echo of it.
//
// A second press from the same ingress is always a second press: repeated keys
// are how a listener skips two tracks, and only the losing transport's echo is
// a duplicate.
inline constexpr std::chrono::milliseconds kSystemMediaCommandEchoWindow{400};

class SystemMediaCommandArbiter {
 public:
  using Clock = std::chrono::steady_clock;

  // True when this ingress owns the press and must act on it.
  bool accept(SystemMediaCommandGroup group, SystemMediaCommandIngress ingress,
              Clock::time_point now = Clock::now()) {
    const std::lock_guard<std::mutex> lock(mutex_);
    Admission& admission = admissions_[systemMediaCommandGroupIndex(group)];
    const bool echo = admission.recorded && admission.ingress != ingress &&
                      now >= admission.when &&
                      now - admission.when <= kSystemMediaCommandEchoWindow;
    if (echo) {
      // The press is accounted for. Forget it so the next press is judged on
      // its own arrival order rather than against a spent admission.
      admission.recorded = false;
      return false;
    }
    admission.recorded = true;
    admission.ingress = ingress;
    admission.when = now;
    return true;
  }

  void reset() {
    const std::lock_guard<std::mutex> lock(mutex_);
    admissions_ = {};
  }

 private:
  struct Admission {
    bool recorded = false;
    SystemMediaCommandIngress ingress = SystemMediaCommandIngress::FocusedSurface;
    Clock::time_point when{};
  };

  std::mutex mutex_;
  std::array<Admission, kSystemMediaCommandGroupCount> admissions_{};
};

// Media-key delivery is a property of the process, not of any one surface: the
// foreground window and the SMTC session are both process-wide. One arbiter
// per process therefore matches what it arbitrates.
inline SystemMediaCommandArbiter& systemMediaCommandArbiter() {
  static SystemMediaCommandArbiter arbiter;
  return arbiter;
}

// Admits a media key observed by a focused Radioify surface. Ordinary keys are
// admitted untouched.
inline bool admitLocalMediaVirtualKey(WORD key) {
  SystemMediaCommandGroup group = SystemMediaCommandGroup::PlayPause;
  if (!systemMediaCommandGroupForVirtualKey(key, &group)) {
    return true;
  }
  return systemMediaCommandArbiter().accept(
      group, SystemMediaCommandIngress::FocusedSurface);
}

// Admits a button press reported by the process-wide SMTC session.
inline bool admitSystemSessionMediaCommand(PlaybackControlCommand command) {
  return systemMediaCommandArbiter().accept(
      systemMediaCommandGroup(command),
      SystemMediaCommandIngress::SystemSession);
}
