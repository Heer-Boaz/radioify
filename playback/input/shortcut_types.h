#pragma once

#include <cstdint>

enum class PlaybackShortcutContext : uint32_t {
  Global = 1u << 0,
  Shared = 1u << 1,
  PlaybackSession = 1u << 2,
    PictureInPicture = 1u << 3,
  ImageViewer = 1u << 4,
};

inline constexpr uint32_t kPlaybackShortcutContextGlobal =
    static_cast<uint32_t>(PlaybackShortcutContext::Global);
inline constexpr uint32_t kPlaybackShortcutContextShared =
    static_cast<uint32_t>(PlaybackShortcutContext::Shared);
inline constexpr uint32_t kPlaybackShortcutContextPlaybackSession =
    static_cast<uint32_t>(PlaybackShortcutContext::PlaybackSession);
inline constexpr uint32_t kPlaybackShortcutContextPictureInPicture =
    static_cast<uint32_t>(PlaybackShortcutContext::PictureInPicture);
inline constexpr uint32_t kPlaybackShortcutContextImageViewer =
    static_cast<uint32_t>(PlaybackShortcutContext::ImageViewer);
inline constexpr uint32_t kPlaybackShortcutContextAll =
    kPlaybackShortcutContextGlobal | kPlaybackShortcutContextShared |
    kPlaybackShortcutContextPlaybackSession |
    kPlaybackShortcutContextPictureInPicture |
    kPlaybackShortcutContextImageViewer;

enum class PlaybackShortcutAction : uint8_t {
  Quit,
  Play,
  Pause,
  TogglePause,
  Stop,
  Previous,
  Next,
  ToggleWindow,
  ToggleFullscreen,
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
    DismissPictureInPicture,
  CloseViewer,
};
