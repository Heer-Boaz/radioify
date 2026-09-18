#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "audio/playback_snapshot.h"
#include "asciiart.h"
#include "input_event.h"
#include "consolescreen.h"
#include "gpu_text_grid.h"
#include "playback/framebuffer/window_presentation.h"
#include "playback/input/command.h"
#include "playback/overlay/overlay.h"
#include "playback/target.h"
#include "playback/video/framebuffer/window/window.h"

class AudioPictureInPictureWindow {
 public:
  explicit AudioPictureInPictureWindow(GpuRuntime& gpu);

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

  struct PlaybackCommand {
    playback_input::Command command;
  };
  struct OpenFiles {
    std::vector<std::filesystem::path> files;
    WindowPlacementState sourcePlacement;
  };
  struct Closed {};
  using Event = std::variant<PlaybackCommand, OpenFiles, Closed>;

  struct PollResult {
    bool windowChanged = false;
    std::vector<Event> events;
  };

  bool isOpen() const;
  const std::string& lastError() const { return lastError_; }
  bool open();
  void close();
  void activate();
  bool toggle();
  NativeWaitHandle inputWaitHandle() const;
  NativeWaitHandle closeRequestedWaitHandle() const;
  void setFileDropAcceptanceEnabled(bool enabled);
  PollResult pollEvents();
  bool render(const Styles& styles, const Context& context);
  WindowPlacementState capturePlacement() const;

 private:
  bool ensureOpen();
  void refreshGridSize();
  void refreshArtwork(const Context& context, int width, int height);
  void drawArtworkBackground(const Styles& styles, int width, int height);
  void handleInput(const InputEvent& ev);
  bool clickControl(playback_overlay::OverlayControlId control);
  void publish(Event event);

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
  std::vector<Event> events_;
  std::string lastError_;
};
