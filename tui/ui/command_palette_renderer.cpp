#include "command_palette_renderer.h"

#include <algorithm>
#include <utility>

#include "unicode_display_width.h"

namespace tui_command_palette {

void draw(ConsoleScreen& screen, Model& model,
          const std::vector<Command>& commands, const Bounds& bounds,
          const Styles& styles) {
  const Layout layout = model.layout(commands, bounds);
  if (!layout.valid) {
    return;
  }

  for (int row = 0; row < layout.height; ++row) {
    screen.writeRun(layout.x, layout.y + row, layout.width, L' ',
                    styles.normal);
  }

  screen.writeChar(layout.x, layout.y, L'+', styles.border);
  screen.writeRun(layout.x + 1, layout.y, layout.width - 2, L'-',
                  styles.border);
  screen.writeChar(layout.x + layout.width - 1, layout.y, L'+',
                   styles.border);
  screen.writeChar(layout.x, layout.y + layout.height - 1, L'+',
                   styles.border);
  screen.writeRun(layout.x + 1, layout.y + layout.height - 1,
                  layout.width - 2, L'-', styles.border);
  screen.writeChar(layout.x + layout.width - 1,
                   layout.y + layout.height - 1, L'+', styles.border);
  for (int row = 1; row < layout.height - 1; ++row) {
    screen.writeChar(layout.x, layout.y + row, L'|', styles.border);
    screen.writeChar(layout.x + layout.width - 1, layout.y + row, L'|',
                     styles.border);
  }

  std::string prompt = "> " + model.query();
  if (utf8DisplayWidth(prompt) > layout.innerWidth) {
    prompt = utf8TakeDisplayWidth(prompt, layout.innerWidth);
  }
  screen.writeText(layout.x + 1, layout.inputY, prompt, styles.normal);

  const std::vector<std::size_t>& filtered =
      model.filteredCommandIndices();
  if (filtered.empty()) {
    std::string noMatches = "(no matches)";
    if (utf8DisplayWidth(noMatches) > layout.innerWidth) {
      noMatches = utf8TakeDisplayWidth(noMatches, layout.innerWidth);
    }
    screen.writeText(layout.x + 1, layout.listY, noMatches, styles.dim);
    return;
  }

  for (int row = 0; row < layout.visibleRows; ++row) {
    const int filteredItem = layout.firstFilteredItem + row;
    if (filteredItem < 0 ||
        filteredItem >= static_cast<int>(filtered.size())) {
      break;
    }
    const std::size_t commandIndex =
        filtered[static_cast<std::size_t>(filteredItem)];
    if (commandIndex >= commands.size()) {
      continue;
    }
    const Command& command = commands[commandIndex];
    std::string left = command.label();
    const std::string& right = command.hotkey();
    const int rightWidth = utf8DisplayWidth(right);
    const int gap = rightWidth > 0 ? 1 : 0;
    const int maximumLeft =
        std::max(0, layout.innerWidth - rightWidth - gap);
    if (utf8DisplayWidth(left) > maximumLeft) {
      left = utf8TakeDisplayWidth(left, maximumLeft);
    }
    const int leftWidth = utf8DisplayWidth(left);
    std::string line = std::move(left);
    if (leftWidth < maximumLeft) {
      line.append(static_cast<std::size_t>(maximumLeft - leftWidth), ' ');
    }
    if (rightWidth > 0) {
      line.push_back(' ');
      line += right;
    }
    screen.writeText(
        layout.x + 1, layout.listY + row, line,
        filteredItem == model.selectedFilteredItem() ? styles.selected
                                                     : styles.normal);
  }

  if (layout.firstFilteredItem > 0) {
    screen.writeChar(layout.x + layout.width - 2, layout.y, L'^',
                     styles.border);
  }
  if (layout.firstFilteredItem + layout.visibleRows <
      static_cast<int>(filtered.size())) {
    screen.writeChar(layout.x + layout.width - 2,
                     layout.y + layout.height - 1, L'v', styles.border);
  }
}

}  // namespace tui_command_palette
