#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "browser_media_menu.h"
#include "command_palette.h"
#include "dialog.h"
#include "shell_command_catalog.h"

class ConsoleScreen;

namespace shell_overlay_stack {

enum class Layer : std::uint8_t {
  None,
  MediaMenu,
  CommandPalette,
  Dialog,
};

struct Bounds {
  int width = 0;
  int height = 0;
  int topInset = 0;
};

struct Interaction {
  bool consumed = false;
  bool changed = false;
  std::optional<tui_browser_media_menu::Command> mediaCommand;
  std::optional<shell_command_catalog::Intent> paletteIntent;
  std::optional<tui_dialog::DialogId> dismissedDialog;
  std::optional<tui_dialog::ButtonActivation> dialogActivation;
};

struct Styles;
class Model;
void draw(ConsoleScreen& screen, Model& model,
          const shell_command_catalog::Catalog& catalog, const Bounds& bounds,
          const Styles& styles);

// Owns the browser shell's transient overlays as one exclusive stack. Opening
// one layer always closes the previous layer, and input is routed only to the
// topmost model.
class Model {
 public:
  Layer activeLayer() const;
  bool active() const { return activeLayer() != Layer::None; }
  bool inputModal() const { return activeLayer() == Layer::Dialog; }
  std::optional<tui_dialog::DialogId> activeDialogId() const {
    return dialog_.activeId();
  }

  bool openMediaMenu(BrowserEntry entry,
                     std::vector<playback_media_actions::Item> items,
                     tui_popup_menu::Anchor anchor = {});
  bool toggleCommandPalette();
  tui_dialog::DialogId openDialog(tui_dialog::Content content);
  bool dismissDialog(tui_dialog::DialogId expectedDialog);
  bool dismiss();

  Interaction handle(const InputEvent& event, const Bounds& bounds,
                     const shell_command_catalog::Catalog& catalog);

 private:
  friend void draw(ConsoleScreen& screen, Model& model,
                   const shell_command_catalog::Catalog& catalog,
                   const Bounds& bounds, const Styles& styles);

  tui_browser_media_menu::Model mediaMenu_;
  tui_command_palette::Model commandPalette_;
  tui_dialog::Model dialog_;
};

}  // namespace shell_overlay_stack
