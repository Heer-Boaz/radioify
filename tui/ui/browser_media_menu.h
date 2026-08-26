#pragma once

#include <optional>
#include <vector>

#include "playback/media_action_catalog.h"
#include "tui/ui/browser_model.h"
#include "tui/ui/popup_menu.h"

namespace tui_popup_menu {
struct Styles;
}

namespace tui_browser_media_menu {

struct Command {
  BrowserEntry entry;
  playback_media_actions::Action action;
};

struct Interaction {
  bool consumed = false;
  bool changed = false;
  bool dismissed = false;
  std::optional<Command> command;
};

class Model;
void draw(ConsoleScreen& screen, Model& model,
          const tui_popup_menu::Bounds& bounds,
          const tui_popup_menu::Styles& styles);

// Owns the subject and actions together so popup selection can never be
// resolved against a different browser entry or a stale parallel action list.
class Model {
 public:
  bool open(BrowserEntry entry,
            std::vector<playback_media_actions::Item> items,
            tui_popup_menu::Anchor anchor = {});
  bool dismiss();

  bool active() const { return popup_.active(); }
  Interaction handle(const InputEvent& event,
                     const tui_popup_menu::Bounds& bounds);

 private:
  friend void draw(ConsoleScreen& screen, Model& model,
                   const tui_popup_menu::Bounds& bounds,
                   const tui_popup_menu::Styles& styles);

  std::optional<BrowserEntry> entry_;
  std::vector<playback_media_actions::Action> actions_;
  tui_popup_menu::Model popup_;
};

}  // namespace tui_browser_media_menu
