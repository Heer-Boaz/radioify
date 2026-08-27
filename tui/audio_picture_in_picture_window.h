#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "audio/playback_snapshot.h"
#include "asciiart.h"
#include "input_event.h"
#include "consolescreen.h"
#include "gpu_text_grid.h"
#include "playback/framebuffer/window_presentation.h"
#include "playback/overlay/overlay.h"
#include "playback/target.h"
#include "playback/video/framebuffer/window/window.h"

class AudioPictureInPictureWindow {
 public:
  struct Styles {
    Style normal;
    Style accent;
    Style dim;
    Style alert;
    Style actionActive;
    Style progressEmpty;
    Style progressFrame;
    Color progressStart;
    Color progressEnd;
  };

  struct Context {
    std::string nowPlayingLabel;
    std::optional<PlaybackTarget> nowPlayingTarget;
    AudioPlaybackSnapshot playback;
  };

  struct Callbacks {
    std::function<void()> onQuit;
    std::function<void()> onTogglePause;
    std::function<void()> onStopPlayback;
    std::function<void()> onPlayPrevious;
    std::function<void()> onPlayNext;
    std::function<void()> onToggleRadio;
    std::function<void()> onToggle50Hz;
    std::function<void(int)> onSeekBy;
    std::function<void(double)> onSeekToRatio;
    std::function<void(float)> onAdjustVolume;
    std::function<bool(const std::vector<std::filesystem::path>&)> onPlayFiles;
    std::function<void()> onClose;
  };

  bool isOpen() const;
  const std::string& lastError() const { return lastError_; }
  bool open();
  void close();
  void activate();
  bool toggle();
  NativeWaitHandle inputWaitHandle() const;
  NativeWaitHandle closeRequestedWaitHandle() const;
  bool pollEvents(const Callbacks& callbacks);
  bool render(const Styles& styles, const Context& context);
  WindowPlacementState capturePlacement() const;

 private:
  bool ensureOpen();
  void refreshGridSize();
  void refreshArtwork(const Context& context, int width, int height);
  void drawArtworkBackground(const Styles& styles, int width, int height);
  void handleInput(const InputEvent& ev, const Callbacks& callbacks);
  bool clickControl(playback_overlay::OverlayControlId control,
                    const Callbacks& callbacks);

  VideoWindow window_;
  ConsoleScreen screen_;
  std::vector<ScreenCell> cells_;
  GpuTextGridFrame frame_;
  std::vector<playback_overlay::OverlayControlSpec> controls_;
  playback_overlay::OverlayCellLayout layout_;
  AsciiArt artwork_;
  std::optional<PlaybackTarget> artworkTarget_;
  int artworkWidth_ = 0;
  int artworkHeight_ = 0;
  bool artworkValid_ = false;
  int hoverControlToken_ = -1;
  int cols_ = 0;
  int rows_ = 0;
  int cellWidth_ = 1;
  int cellHeight_ = 1;
  playback_overlay::InteractionMap interactions_;
  std::string lastError_;
};
