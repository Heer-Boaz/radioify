#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "tui/input_event.h"

namespace tui_button_row {

using ButtonId = std::uint32_t;

struct Button {
  ButtonId id = 0;
  std::string label;
};

struct Placement {
  std::size_t index = 0;
  int x = 0;
  int width = 0;
};

struct Layout {
  int y = -1;
  std::vector<Placement> buttons;
};

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

// Centers one row of buttons inside the supplied horizontal bounds. Layout,
// hit-testing and keyboard selection are shared by modal dialogs and non-modal
// panels so both surfaces retain one interaction grammar.
Layout layout(const std::vector<Button>& buttons, int x, int width, int y);
std::optional<std::size_t> hitTest(const Layout& layout, int x, int y);
std::size_t selectAdjacent(std::size_t selected, std::size_t buttonCount,
                           int direction);

}  // namespace tui_button_row
