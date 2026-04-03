#pragma once

enum class PlaybackLayout {
  Window,
  Terminal,
};

enum class PlaybackRenderMode {
  AsciiTerminal,
  Other,
};

inline PlaybackRenderMode resolvePlaybackMode(bool enableAscii, bool windowEnabled) {
  PlaybackLayout layout =
      windowEnabled ? PlaybackLayout::Window : PlaybackLayout::Terminal;
  bool terminalAscii =
      (layout == PlaybackLayout::Terminal) && static_cast<bool>(enableAscii);
  return terminalAscii ? PlaybackRenderMode::AsciiTerminal : PlaybackRenderMode::Other;
}

inline bool isAsciiPlaybackMode(PlaybackRenderMode mode) {
  return mode == PlaybackRenderMode::AsciiTerminal;
}
