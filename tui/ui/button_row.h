#pragma once

#include <optional>

#include "tui/input_event.h"
#include "ui/text_grid/button_layout.h"

namespace tui_button_row {

using ButtonId = text_grid_button_layout::ButtonId;
using Button = text_grid_button_layout::Button;
using Placement = text_grid_button_layout::Placement;
using Layout = text_grid_button_layout::Layout;
using text_grid_button_layout::hitTest;
using text_grid_button_layout::labelFor;
using text_grid_button_layout::layout;
using text_grid_button_layout::responsiveLayout;
using text_grid_button_layout::selectAdjacent;

struct PointerInteraction {
  bool changed = false;
  // True while this event belongs to a press that began on a button. Callers
  // use this as pointer capture so releasing outside cannot activate content
  // underneath the button row.
  bool captured = false;
  std::optional<std::size_t> activated;
};

enum class KeyboardAction {
  None,
  Activate,
  SelectPrevious,
  SelectNext,
  FocusPrevious,
  FocusNext,
  Dismiss,
};

// Implements the desktop button contract shared by dialogs and panels: arm on
// left-button press, activate only when that same press is released over the
// same button, and cancel the gesture when pointer ownership is lost.
class PointerState {
 public:
  PointerInteraction handle(const InputEvent& event, const Layout& layout);
  void reset();

  std::optional<std::size_t> hovered() const { return hovered_; }

 private:
  std::optional<std::size_t> hovered_;
  std::optional<std::size_t> armed_;
};

KeyboardAction resolveKeyboardAction(const KeyEvent& key);

}  // namespace tui_button_row
