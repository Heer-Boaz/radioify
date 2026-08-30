#pragma once

#include <windows.h>

#include <cstdint>

// Exactly one process-level ingress owns commands emitted by Windows media
// controls. LocalInputFallback covers whichever Radioify surface currently has
// keyboard focus; SystemMediaTransportControls covers the process-wide SMTC
// callback registered by PlaybackSystemControls.
enum class SystemMediaCommandOwner : std::uint8_t {
  LocalInputFallback,
  SystemMediaTransportControls,
};

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

inline constexpr bool localInputOwnsSystemMediaCommands(
    SystemMediaCommandOwner owner) {
  return owner == SystemMediaCommandOwner::LocalInputFallback;
}

inline constexpr bool shouldDispatchLocalVirtualKey(
    WORD key, SystemMediaCommandOwner owner) {
  return !isSystemMediaVirtualKey(key) ||
         localInputOwnsSystemMediaCommands(owner);
}
