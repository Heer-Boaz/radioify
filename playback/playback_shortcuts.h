#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>

#include "consoleinput.h"
#include "playback/playback_media_keys.h"
#include "playback/playback_shortcut_types.h"

struct PlaybackShortcutBinding {
  PlaybackShortcutAction action = PlaybackShortcutAction::TogglePause;
  WORD vk = 0;
  char lower = 0;
  char upper = 0;
  DWORD requiredControlMask = 0;
  DWORD forbiddenControlMask = 0;
};

struct PlaybackShortcutBindingSet {
  const PlaybackShortcutBinding* bindings = nullptr;
  size_t count = 0;
  uint32_t contexts = kPlaybackShortcutContextAll;
};

inline constexpr DWORD kPlaybackShortcutTextForbiddenMask =
    LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED | LEFT_ALT_PRESSED |
    RIGHT_ALT_PRESSED;
inline constexpr DWORD kPlaybackShortcutChordForbiddenMask =
    LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED | SHIFT_PRESSED;

inline bool matchesAsciiShortcut(const KeyEvent& key, WORD vk, char lower,
                                 char upper) {
  return key.vk == vk || key.ch == lower || key.ch == upper;
}

inline bool matchesShortcut(const KeyEvent& key, WORD vk, char lower,
                            char upper, DWORD requiredControlMask = 0,
                            DWORD forbiddenControlMask = 0) {
  if (requiredControlMask != 0 &&
      (key.control & requiredControlMask) != requiredControlMask) {
    return false;
  }
  if (forbiddenControlMask != 0 &&
      (key.control & forbiddenControlMask) != 0) {
    return false;
  }
  return matchesAsciiShortcut(key, vk, lower, upper);
}

// Base playback bindings are shared across modes. Context-specific sets are
// resolved first so they can override or extend this list.
inline constexpr std::array<PlaybackShortcutBinding, 20>
    kPlaybackSharedShortcutBindings = {{
        {PlaybackShortcutAction::Quit, 'Q', 'q', 'Q',
         LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED, kPlaybackShortcutChordForbiddenMask},
        {PlaybackShortcutAction::Play, kPlaybackVkMediaPlay, 0, 0, 0, 0},
        {PlaybackShortcutAction::Pause, kPlaybackVkMediaPause, 0, 0, 0, 0},
        {PlaybackShortcutAction::TogglePause, VK_SPACE, ' ', ' ',
         0, kPlaybackShortcutTextForbiddenMask},
        {PlaybackShortcutAction::TogglePause, VK_MEDIA_PLAY_PAUSE, 0, 0, 0, 0},
        {PlaybackShortcutAction::Stop, VK_MEDIA_STOP, 0, 0, 0, 0},
        {PlaybackShortcutAction::Previous, VK_MEDIA_PREV_TRACK, 0, 0, 0, 0},
        {PlaybackShortcutAction::Next, VK_MEDIA_NEXT_TRACK, 0, 0, 0, 0},
        {PlaybackShortcutAction::ToggleWindow, 'W', 'w', 'W', 0,
         kPlaybackShortcutTextForbiddenMask},
        {PlaybackShortcutAction::ToggleRadio, 'R', 'r', 'R', 0,
         kPlaybackShortcutTextForbiddenMask},
        {PlaybackShortcutAction::Toggle50Hz, 'H', 'h', 'H', 0,
         kPlaybackShortcutTextForbiddenMask},
        {PlaybackShortcutAction::ToggleSubtitles, 'S', 's', 'S', 0,
         kPlaybackShortcutTextForbiddenMask},
        {PlaybackShortcutAction::ToggleAudioTrack, 'A', 'a', 'A', 0,
         kPlaybackShortcutTextForbiddenMask},
        {PlaybackShortcutAction::ToggleOptions, 'O', 'o', 'O', 0,
         kPlaybackShortcutTextForbiddenMask},
        {PlaybackShortcutAction::SeekBackward, VK_OEM_4, '[', '[', 0,
         kPlaybackShortcutTextForbiddenMask},
        {PlaybackShortcutAction::SeekForward, VK_OEM_6, ']', ']', 0,
         kPlaybackShortcutTextForbiddenMask},
        {PlaybackShortcutAction::SeekBackward, VK_LEFT, 0, 0,
         LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED, kPlaybackShortcutChordForbiddenMask},
        {PlaybackShortcutAction::SeekForward, VK_RIGHT, 0, 0,
         LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED, kPlaybackShortcutChordForbiddenMask},
        {PlaybackShortcutAction::VolumeUp, VK_UP, 0, 0, SHIFT_PRESSED,
         LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED | LEFT_ALT_PRESSED |
             RIGHT_ALT_PRESSED},
        {PlaybackShortcutAction::VolumeDown, VK_DOWN, 0, 0, SHIFT_PRESSED,
         LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED | LEFT_ALT_PRESSED |
             RIGHT_ALT_PRESSED},
    }};

inline constexpr std::array<PlaybackShortcutBinding, 4>
    kPlaybackSessionShortcutBindings = {{
        {PlaybackShortcutAction::TogglePictureInPicture, 'P', 'p', 'P', 0,
         kPlaybackShortcutTextForbiddenMask},
        {PlaybackShortcutAction::ExitPlaybackSession, VK_ESCAPE, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask},
        {PlaybackShortcutAction::ExitPlaybackSession, VK_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask},
        {PlaybackShortcutAction::ExitPlaybackSession, VK_BROWSER_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask},
    }};

inline constexpr std::array<PlaybackShortcutBinding, 4>
    kAudioMiniPlayerShortcutBindings = {{
        {PlaybackShortcutAction::DismissMiniPlayer, VK_ESCAPE, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask},
        {PlaybackShortcutAction::DismissMiniPlayer, VK_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask},
        {PlaybackShortcutAction::DismissMiniPlayer, VK_BROWSER_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask},
        {PlaybackShortcutAction::DismissMiniPlayer, 'P', 'p', 'P', 0,
         kPlaybackShortcutTextForbiddenMask},
    }};

inline std::optional<PlaybackShortcutAction> resolvePlaybackShortcutAction(
    const KeyEvent& key, uint32_t shortcutContexts,
    std::initializer_list<PlaybackShortcutBindingSet> bindingSets) {
  for (const PlaybackShortcutBindingSet& set : bindingSets) {
    if ((set.contexts & shortcutContexts) == 0 || !set.bindings ||
        set.count == 0) {
      continue;
    }
    for (size_t i = 0; i < set.count; ++i) {
      const PlaybackShortcutBinding& binding = set.bindings[i];
      if (matchesShortcut(key, binding.vk, binding.lower, binding.upper,
                          binding.requiredControlMask,
                          binding.forbiddenControlMask)) {
        return binding.action;
      }
    }
  }
  return std::nullopt;
}

inline std::optional<PlaybackShortcutAction> resolvePlaybackShortcutAction(
    const KeyEvent& key, uint32_t shortcutContexts = kPlaybackShortcutContextShared) {
  return resolvePlaybackShortcutAction(
      key, shortcutContexts,
      {{kPlaybackSessionShortcutBindings.data(),
        kPlaybackSessionShortcutBindings.size(),
        kPlaybackShortcutContextPlaybackSession},
       {kAudioMiniPlayerShortcutBindings.data(),
        kAudioMiniPlayerShortcutBindings.size(),
        kPlaybackShortcutContextAudioMiniPlayer},
       {kPlaybackSharedShortcutBindings.data(),
        kPlaybackSharedShortcutBindings.size(),
        kPlaybackShortcutContextShared}});
}
