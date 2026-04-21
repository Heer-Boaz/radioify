#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "consoleinput.h"
#include "playback/playback_media_keys.h"
#include "playback/playback_shortcut_types.h"
#include "shortcut_match.h"

struct PlaybackShortcutBinding {
  PlaybackShortcutAction action = PlaybackShortcutAction::TogglePause;
  WORD vk = 0;
  char lower = 0;
  char upper = 0;
  DWORD requiredModifierMask = 0;
  DWORD forbiddenModifierMask = 0;
  uint32_t contexts = kPlaybackShortcutContextAll;
};

inline constexpr DWORD kPlaybackShortcutCtrlMask = kShortcutCtrlMask;
inline constexpr DWORD kPlaybackShortcutAltMask = kShortcutAltMask;
inline constexpr DWORD kPlaybackShortcutShiftMask = kShortcutShiftMask;
inline constexpr DWORD kPlaybackShortcutTextForbiddenMask =
    kShortcutTextForbiddenMask;
inline constexpr DWORD kPlaybackShortcutChordForbiddenMask =
    kShortcutChordForbiddenMask;
inline constexpr DWORD kPlaybackShortcutSeekForbiddenMask =
    kPlaybackShortcutTextForbiddenMask | kPlaybackShortcutShiftMask;

// One shared shortcut table. Context masks let modes layer additional keys on
// top of the shared map without owning separate per-mode tables.
inline constexpr std::array<PlaybackShortcutBinding, 34>
    kPlaybackShortcutBindings = {{
        {PlaybackShortcutAction::Quit, 'Q', 'q', 'Q', kPlaybackShortcutCtrlMask,
         kPlaybackShortcutChordForbiddenMask, kPlaybackShortcutContextGlobal},
        {PlaybackShortcutAction::TogglePictureInPicture, 'P', 'p', 'P', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextPlaybackSession},
        {PlaybackShortcutAction::ExitPlaybackSession, VK_ESCAPE, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextPlaybackSession},
        {PlaybackShortcutAction::ExitPlaybackSession, VK_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextPlaybackSession},
        {PlaybackShortcutAction::ExitPlaybackSession, VK_BROWSER_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextPlaybackSession},
        {PlaybackShortcutAction::DismissMiniPlayer, VK_ESCAPE, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextAudioMiniPlayer},
        {PlaybackShortcutAction::DismissMiniPlayer, VK_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextAudioMiniPlayer},
        {PlaybackShortcutAction::DismissMiniPlayer, VK_BROWSER_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextAudioMiniPlayer},
        {PlaybackShortcutAction::DismissMiniPlayer, 'P', 'p', 'P', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextAudioMiniPlayer},
        {PlaybackShortcutAction::CloseViewer, VK_ESCAPE, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextImageViewer},
        {PlaybackShortcutAction::CloseViewer, VK_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextImageViewer},
        {PlaybackShortcutAction::CloseViewer, VK_BROWSER_BACK, 0, 0, 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextImageViewer},
        {PlaybackShortcutAction::Play, kPlaybackVkMediaPlay, 0, 0, 0, 0,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::Pause, kPlaybackVkMediaPause, 0, 0, 0, 0,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::TogglePause, VK_SPACE, ' ', ' ',
         0, kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::TogglePause, VK_MEDIA_PLAY_PAUSE, 0, 0, 0, 0,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::Stop, VK_MEDIA_STOP, 0, 0, 0, 0,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::Previous, VK_MEDIA_PREV_TRACK, 0, 0, 0,
         0, kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::Next, VK_MEDIA_NEXT_TRACK, 0, 0, 0, 0,
         kPlaybackShortcutContextShared},
        // Shared navigation layer:
        //   - Left/Right arrows seek within the current item.
        //   - Ctrl+Left/Right move to the previous/next item in the playlist.
        {PlaybackShortcutAction::Previous, VK_LEFT, 0, 0,
         kPlaybackShortcutCtrlMask, kPlaybackShortcutChordForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::Next, VK_RIGHT, 0, 0, kPlaybackShortcutCtrlMask,
         kPlaybackShortcutChordForbiddenMask, kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::ToggleWindow, 'W', 'w', 'W', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::ToggleFullscreen, VK_RETURN, 0, 0,
         kPlaybackShortcutAltMask,
         kPlaybackShortcutCtrlMask | kPlaybackShortcutShiftMask,
         kPlaybackShortcutContextPlaybackSession},
        {PlaybackShortcutAction::ToggleRadio, 'R', 'r', 'R', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::Toggle50Hz, 'H', 'h', 'H', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::ToggleSubtitles, 'S', 's', 'S', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::ToggleAudioTrack, 'A', 'a', 'A', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::ToggleOptions, 'O', 'o', 'O', 0,
         kPlaybackShortcutTextForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::SeekBackward, VK_OEM_4, '[', '[', 0,
         kPlaybackShortcutSeekForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::SeekForward, VK_OEM_6, ']', ']', 0,
         kPlaybackShortcutSeekForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::SeekBackward, VK_LEFT, 0, 0, 0,
         kPlaybackShortcutSeekForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::SeekForward, VK_RIGHT, 0, 0, 0,
         kPlaybackShortcutSeekForbiddenMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::VolumeUp, VK_UP, 0, 0, kPlaybackShortcutShiftMask,
         kPlaybackShortcutCtrlMask | kPlaybackShortcutAltMask,
         kPlaybackShortcutContextShared},
        {PlaybackShortcutAction::VolumeDown, VK_DOWN, 0, 0,
         kPlaybackShortcutShiftMask,
         kPlaybackShortcutCtrlMask | kPlaybackShortcutAltMask,
         kPlaybackShortcutContextShared},
    }};

inline std::optional<PlaybackShortcutAction> resolvePlaybackShortcutAction(
    const KeyEvent& key,
    uint32_t shortcutContexts = kPlaybackShortcutContextGlobal |
                                kPlaybackShortcutContextShared) {
  for (const PlaybackShortcutBinding& binding : kPlaybackShortcutBindings) {
    if ((binding.contexts & shortcutContexts) == 0) {
      continue;
    }
    if (matchesShortcut(key, binding.vk, binding.lower, binding.upper,
                        binding.requiredModifierMask,
                        binding.forbiddenModifierMask)) {
      return binding.action;
    }
  }
  return std::nullopt;
}
