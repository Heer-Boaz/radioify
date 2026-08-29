#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "consoleinput.h"

class VideoWindow;

enum class ApplicationInputSurface : std::uint8_t {
  Terminal,
  ShellWindow,
  PlaybackWindow,
};

class FairInputScheduler {
 public:
  template <typename Poll>
  bool pollNext(Poll&& poll, InputEvent& out,
                ApplicationInputSurface& source) {
    for (std::size_t offset = 0; offset < kOrder.size(); ++offset) {
      const std::size_t index = (nextIndex_ + offset) % kOrder.size();
      if (poll(kOrder[index], out)) {
        source = kOrder[index];
        nextIndex_ = (index + 1) % kOrder.size();
        return true;
      }
    }
    return false;
  }

 private:
  static constexpr std::array<ApplicationInputSurface, 3> kOrder{
      ApplicationInputSurface::Terminal, ApplicationInputSurface::ShellWindow,
      ApplicationInputSurface::PlaybackWindow};
  std::size_t nextIndex_ = 1;
};

class ConsoleInputPump {
 public:
  bool pollNext(ConsoleInput& input, InputEvent& out);

 private:
  std::optional<InputEvent> queued_;
};

// Arbitrates terminal, shell-window and playback-window input as peer sources.
// At most one event is delivered per application turn and priority rotates
// after delivery, so no busy pointer stream can starve Cancel, Quit or resize.
class ApplicationInputPump {
 public:
  bool pollNext(ConsoleInput& console, VideoWindow* shellWindow,
                InputEvent& out, ApplicationInputSurface& source);

  template <typename PlaybackPoll>
  bool pollNext(ConsoleInput& console, VideoWindow* shellWindow,
                PlaybackPoll&& pollPlayback, InputEvent& out,
                ApplicationInputSurface& source) {
    return scheduler_.pollNext(
        [&](ApplicationInputSurface candidate, InputEvent& event) {
          switch (candidate) {
            case ApplicationInputSurface::Terminal:
              return console_.pollNext(console, event);
            case ApplicationInputSurface::ShellWindow:
              return pollShellWindow(shellWindow, event);
            case ApplicationInputSurface::PlaybackWindow:
              return pollPlayback(event);
          }
          return false;
        },
        out, source);
  }

 private:
  static bool pollShellWindow(VideoWindow* window, InputEvent& out);

  ConsoleInputPump console_;
  FairInputScheduler scheduler_;
};
