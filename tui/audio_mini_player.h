#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "asciiart.h"
#include "consoleinput.h"
#include "consolescreen.h"
#include "gpu_text_grid.h"
#include "playback/overlay/overlay.h"
#include "playback/session/state.h"
#include "playback/video/framebuffer/window/window.h"

class AudioMiniPlayer {
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
    std::filesystem::path nowPlayingPath;
    int trackIndex = -1;
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
  bool openWithPlacement(const WindowPlacementState& placement);
  void close();
  bool toggle();
  bool pollEvents(const Callbacks& callbacks);
  bool render(const Styles& styles, const Context& context);
  WindowPlacementState capturePlacement() const;

 private:
  bool open(const WindowPlacementState* initialPlacement);
  bool ensureOpen();
  void refreshGridSize();
  void refreshArtwork(const Context& context, int width, int height);
  void drawArtworkBackground(const Styles& styles, int width, int height);
  void updateInteractiveRects();
  void holdSeekDisplay(double targetSec);
  void handleInput(const InputEvent& ev, const Callbacks& callbacks);
  bool clickControl(playback_overlay::OverlayControlId control,
                    const Callbacks& callbacks);
  int controlAt(int x, int y) const;

  VideoWindow window_;
  ConsoleScreen screen_;
  std::vector<ScreenCell> cells_;
  GpuTextGridFrame frame_;
  std::vector<playback_overlay::OverlayControlSpec> controls_;
  playback_overlay::OverlayCellLayout layout_;
  AsciiArt artwork_;
  std::filesystem::path artworkPath_;
  int artworkTrackIndex_ = -2;
  int artworkWidth_ = 0;
  int artworkHeight_ = 0;
  bool artworkValid_ = false;
  int hoverIndex_ = -1;
  int cols_ = 0;
  int rows_ = 0;
  int cellWidth_ = 1;
  int cellHeight_ = 1;
  int progressX_ = -1;
  int progressY_ = -1;
  int progressWidth_ = 0;
  double seekDisplaySec_ = -1.0;
  bool seekHoldActive_ = false;
  std::chrono::steady_clock::time_point seekHoldStart_{};
  std::string lastError_;
};
