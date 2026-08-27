#include "browser_action_strip_renderer.h"

#include <algorithm>
#include <string>

#include "core/unicode_display_width.h"

namespace browser_action_strip {

ActionStripLayout draw(ConsoleScreen& screen, const std::vector<Item>& items,
                       int width, int height, int top, int hoveredIndex,
                       const Styles& styles) {
  ActionStripLayout interactions;
  const Layout actionLayout = layout(items, width, top);
  if (actionLayout.placements.empty()) return interactions;

  interactions.y = top;
  interactions.buttons.reserve(actionLayout.placements.size());
  for (const Placement& placement : actionLayout.placements) {
    if (placement.y >= height || placement.itemIndex >= items.size()) break;
    const Item& item = items[placement.itemIndex];
    const bool hovered =
        hoveredIndex == static_cast<int>(interactions.buttons.size());
    std::string text = hovered ? item.hoverLabel : item.label;
    int textWidth = utf8DisplayWidth(text);
    if (textWidth > placement.width) {
      text = utf8TakeDisplayWidth(text, placement.width);
      textWidth = utf8DisplayWidth(text);
    }
    if (textWidth < placement.width) {
      text.append(static_cast<std::size_t>(placement.width - textWidth), ' ');
    }
    interactions.buttons.push_back(
        {item.id, placement.x, placement.x + placement.width, placement.y});
    screen.writeText(placement.x, placement.y, text,
                     item.active ? styles.active : styles.normal);
  }
  return interactions;
}

}  // namespace browser_action_strip
