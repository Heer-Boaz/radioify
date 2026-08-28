#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace text_grid_button_layout {

using ButtonId = std::uint32_t;

struct Button {
  Button() = default;
  Button(ButtonId buttonId, std::string fullLabel,
         std::string constrainedLabel = {})
      : id(buttonId),
        label(std::move(fullLabel)),
        compactLabel(std::move(constrainedLabel)) {}

  ButtonId id = 0;
  std::string label;
  // Optional wording for constrained surfaces. It must remain an explicit,
  // recognizable action label; layout never truncates either spelling into
  // an active hit target.
  std::string compactLabel;
};

struct Placement {
  std::size_t index = 0;
  int x = 0;
  int width = 0;
  int y = -1;
  bool compact = false;
};

struct Layout {
  int y = -1;
  int rowCount = 0;
  std::vector<Placement> buttons;
};

// Pure text-grid geometry shared by browser and playback adapters. It has no
// terminal input or rendering dependencies.
Layout layout(const std::vector<Button>& buttons, int x, int width, int y);
Layout responsiveLayout(const std::vector<Button>& buttons, int x, int width,
                        int bottomY, int maxRows);

const std::string& labelFor(const Button& button, const Placement& placement);
std::optional<std::size_t> hitTest(const Layout& layout, int x, int y);
std::size_t selectAdjacent(std::size_t selected, std::size_t buttonCount,
                           int direction);

}  // namespace text_grid_button_layout
