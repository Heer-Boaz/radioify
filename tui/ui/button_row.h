#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

// Centers one row of buttons inside the supplied horizontal bounds. Layout,
// hit-testing and keyboard selection are shared by modal dialogs and non-modal
// panels so both surfaces retain one interaction grammar.
Layout layout(const std::vector<Button>& buttons, int x, int width, int y);
std::optional<std::size_t> hitTest(const Layout& layout, int x, int y);
std::size_t selectAdjacent(std::size_t selected, std::size_t buttonCount,
                           int direction);

}  // namespace tui_button_row
