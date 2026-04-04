#pragma once

enum class PlaybackLayout {
  Window,
  Terminal,
};

enum class PlaybackRenderMode {
  AsciiTerminal,
  Other,
};

inline bool isWindowPlaybackLayout(PlaybackLayout layout) {
  return layout == PlaybackLayout::Window;
}

inline PlaybackLayout togglePlaybackLayout(PlaybackLayout layout) {
  return isWindowPlaybackLayout(layout) ? PlaybackLayout::Terminal
                                        : PlaybackLayout::Window;
}

inline PlaybackRenderMode resolvePlaybackMode(bool enableAscii,
                                              PlaybackLayout layout) {
  bool terminalAscii =
      (layout == PlaybackLayout::Terminal) && static_cast<bool>(enableAscii);
  return terminalAscii ? PlaybackRenderMode::AsciiTerminal : PlaybackRenderMode::Other;
}

inline bool isAsciiPlaybackMode(PlaybackRenderMode mode) {
  return mode == PlaybackRenderMode::AsciiTerminal;
}
