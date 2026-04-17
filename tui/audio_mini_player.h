#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "consoleinput.h"
#include "consolescreen.h"
#include "gpu_text_grid.h"
#include "videowindow.h"

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
    bool melodyVisualizationEnabled = false;
  };

  struct Callbacks {
    std::function<void()> onTogglePause;
    std::function<void()> onStopPlayback;
    std::function<void()> onPlayPrevious;
    std::function<void()> onPlayNext;
    std::function<void()> onToggleRadio;
    std::function<void()> onToggle50Hz;
    std::function<void()> onToggleMelodyVisualization;
    std::function<void(int)> onSeekBy;
    std::function<void(double)> onSeekToRatio;
    std::function<void(float)> onAdjustVolume;
    std::function<void()> onClose;
  };

  bool isOpen() const;
  bool open();
  void close();
  bool toggle();
  bool pollEvents(const Callbacks& callbacks);
  bool render(const Styles& styles, const Context& context);

 private:
  struct Button {
    enum class Action {
      Previous,
      PlayPause,
      Next,
      Stop,
      Radio,
      Melody,
      Hz50,
      Close,
    };

    Action action = Action::PlayPause;
    int x0 = 0;
    int y = 0;
    int x1 = 0;
  };

  bool ensureOpen();
  void refreshGridSize();
  void handleInput(const InputEvent& ev, const Callbacks& callbacks);
  bool clickButton(const Button& button, const Callbacks& callbacks);
  double progressRatioAt(int x) const;

  VideoWindow window_;
  ConsoleScreen screen_;
  std::vector<ScreenCell> cells_;
  GpuTextGridFrame frame_;
  std::vector<Button> buttons_;
  int cols_ = 0;
  int rows_ = 0;
  int cellWidth_ = 1;
  int cellHeight_ = 1;
  int progressX_ = -1;
  int progressY_ = -1;
  int progressWidth_ = 0;
};
