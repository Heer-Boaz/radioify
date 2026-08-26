#include "popup_menu_renderer.h"

#include <algorithm>

#include "unicode_display_width.h"

namespace tui_popup_menu {

void draw(ConsoleScreen& screen, Model& model, const Bounds& bounds,
          const Styles& styles) {
  const Layout layout = model.layout(bounds);
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

  const std::vector<std::string>& labels = model.labels();
  const int innerWidth = std::max(1, layout.width - 2);
  for (int row = 0; row < layout.visibleRows; ++row) {
    const int item = layout.firstItem + row;
    if (item < 0 || item >= static_cast<int>(labels.size())) {
      continue;
    }
    std::string text = " " + labels[static_cast<std::size_t>(item)];
    if (utf8DisplayWidth(text) > innerWidth) {
      text = utf8TakeDisplayWidth(text, innerWidth);
    }
    const int textWidth = utf8DisplayWidth(text);
    if (textWidth < innerWidth) {
      text.append(static_cast<std::size_t>(innerWidth - textWidth), ' ');
    }
    screen.writeText(layout.x + 1, layout.listY + row, text,
                     item == model.selected() ? styles.selected
                                              : styles.normal);
  }

  if (layout.firstItem > 0) {
    screen.writeChar(layout.x + layout.width - 2, layout.y, L'^',
                     styles.border);
  }
  if (layout.firstItem + layout.visibleRows <
      static_cast<int>(labels.size())) {
    screen.writeChar(layout.x + layout.width - 2,
                     layout.y + layout.height - 1, L'v', styles.border);
  }
}

}  // namespace tui_popup_menu
