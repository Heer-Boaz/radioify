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

struct DialogOwner {
  std::uint64_t value = 0;

  constexpr explicit operator bool() const { return value != 0; }
  friend constexpr bool operator==(DialogOwner left, DialogOwner right) {
    return left.value == right.value;
  }
};

struct DialogLease {
  DialogOwner owner;
  tui_dialog::DialogId dialog;

  constexpr explicit operator bool() const {
    return static_cast<bool>(owner) && static_cast<bool>(dialog);
  }
  friend constexpr bool operator==(const DialogLease& left,
                                   const DialogLease& right) {
    return left.owner == right.owner && left.dialog == right.dialog;
  }
};

struct DialogOpening {
  DialogLease lease;
  std::optional<DialogLease> replaced;
};

struct DialogResolution {
  DialogLease lease;
  std::optional<tui_dialog::ButtonId> activatedButton;
};

struct Dismissal {
  bool changed = false;
  std::optional<DialogLease> dialog;
};

struct Interaction {
  bool consumed = false;
  bool changed = false;
  std::optional<tui_browser_media_menu::Command> mediaCommand;
  std::optional<shell_command_catalog::Intent> paletteIntent;
  std::optional<DialogResolution> dialogResolution;
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
  DialogOwner createDialogOwner();
  std::optional<DialogLease> activeDialog() const;

  bool openMediaMenu(BrowserEntry entry,
                     std::vector<playback_media_actions::Item> items,
                     tui_popup_menu::Anchor anchor = {});
  bool toggleCommandPalette();
  DialogOpening openDialog(DialogOwner owner, tui_dialog::Content content);
  std::optional<DialogLease> dismissDialog(
      tui_dialog::DialogId expectedDialog);
  Dismissal dismiss();

  Interaction handle(const InputEvent& event, const Bounds& bounds,
                     const shell_command_catalog::Catalog& catalog);

 private:
  friend void draw(ConsoleScreen& screen, Model& model,
                   const shell_command_catalog::Catalog& catalog,
                   const Bounds& bounds, const Styles& styles);

  tui_browser_media_menu::Model mediaMenu_;
  tui_command_palette::Model commandPalette_;
  tui_dialog::Model dialog_;
  std::optional<DialogOwner> dialogOwner_;
  std::uint64_t nextDialogOwnerValue_ = 1;
};

}  // namespace shell_overlay_stack
