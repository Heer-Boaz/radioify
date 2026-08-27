#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "browser_model.h"
#include "command_palette.h"
#include "playback/input/shortcut_types.h"

namespace shell_command_catalog {

struct SetBrowserView {
  BrowserState::ViewMode mode = BrowserState::ViewMode::Thumbnails;
};

struct RevealPlayingFile {};

using Intent = std::variant<PlaybackAction, SetBrowserView, RevealPlayingFile>;

struct Context {
  bool videoActive = false;
  bool audioAvailable = false;
  bool pictureInPictureOpen = false;
  bool supports50Hz = false;
  bool pitchMonitorActive = false;
  bool optionsAvailable = false;
  bool currentTargetAvailable = false;
};

class Catalog {
 public:
  const std::vector<tui_command_palette::Command>& commands() const {
    return commands_;
  }

  const Intent* intentAt(std::size_t index) const {
    return index < intents_.size() ? &intents_[index] : nullptr;
  }

 private:
  friend Catalog build(const Context& context);

  template <typename IntentValue>
  void add(std::string label, std::string hotkey, IntentValue intent) {
    commands_.emplace_back(std::move(label), std::move(hotkey));
    intents_.emplace_back(std::move(intent));
  }

  std::vector<tui_command_palette::Command> commands_;
  std::vector<Intent> intents_;
};

Catalog build(const Context& context);

}  // namespace shell_command_catalog
