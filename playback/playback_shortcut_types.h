#pragma once

#include <cstdint>

enum class PlaybackShortcutContext : uint32_t {
  Shared = 1u << 0,
  PlaybackSession = 1u << 1,
  AudioMiniPlayer = 1u << 2,
};

inline constexpr uint32_t kPlaybackShortcutContextShared =
    static_cast<uint32_t>(PlaybackShortcutContext::Shared);
inline constexpr uint32_t kPlaybackShortcutContextPlaybackSession =
    static_cast<uint32_t>(PlaybackShortcutContext::PlaybackSession);
inline constexpr uint32_t kPlaybackShortcutContextAudioMiniPlayer =
    static_cast<uint32_t>(PlaybackShortcutContext::AudioMiniPlayer);
inline constexpr uint32_t kPlaybackShortcutContextAll =
    kPlaybackShortcutContextShared | kPlaybackShortcutContextPlaybackSession |
    kPlaybackShortcutContextAudioMiniPlayer;

enum class PlaybackShortcutAction : uint8_t {
  Quit,
  Play,
  Pause,
  TogglePause,
  Stop,
  Previous,
  Next,
  ToggleWindow,
  ToggleRadio,
  Toggle50Hz,
  ToggleSubtitles,
  ToggleAudioTrack,
  ToggleOptions,
  SeekBackward,
  SeekForward,
  VolumeUp,
  VolumeDown,
  TogglePictureInPicture,
  ExitPlaybackSession,
  DismissMiniPlayer,
};
