#include "playback/overlay/context_menu.h"

#include <algorithm>
#include <cmath>

#include "unicode_display_width.h"

namespace playback_overlay {

ContextMenuCellLayout layoutContextMenuCells(
    const ContextMenuSnapshot& menu, int surfaceWidth, int surfaceHeight) {
  ContextMenuCellLayout layout;
  if (!menu.visible || menu.items.empty() || surfaceWidth < 3 ||
      surfaceHeight < 3) {
    return layout;
  }

  const int itemCount = static_cast<int>(menu.items.size());
  const int visibleRows = std::min(itemCount, surfaceHeight - 2);
  int selectedIndex = 0;
  if (menu.selectedItem) {
    const auto selected = std::find_if(
        menu.items.begin(), menu.items.end(), [&](const ContextMenuItem& item) {
          return item.token == *menu.selectedItem;
        });
    if (selected != menu.items.end()) {
      selectedIndex = static_cast<int>(selected - menu.items.begin());
    }
  }
  const int firstVisible = std::clamp(
      selectedIndex - visibleRows + 1, 0, itemCount - visibleRows);
  int labelWidth = 1;
  for (const ContextMenuItem& item : menu.items) {
    labelWidth = std::max(
        labelWidth, utf8DisplayWidth(item.label));
  }
  const int innerWidth = std::min(surfaceWidth - 2, labelWidth + 2);
  layout.width = innerWidth + 2;
  layout.height = visibleRows + 2;

  const double xRatio =
      std::clamp(std::isfinite(menu.anchorXRatio) ? menu.anchorXRatio : 0.5,
                 0.0, 1.0);
  const double yRatio =
      std::clamp(std::isfinite(menu.anchorYRatio) ? menu.anchorYRatio : 0.5,
                 0.0, 1.0);
  const int anchorX = static_cast<int>(
      std::llround(xRatio * static_cast<double>(surfaceWidth - 1)));
  const int anchorY = static_cast<int>(
      std::llround(yRatio * static_cast<double>(surfaceHeight - 1)));
  layout.x = std::clamp(anchorX, 0, surfaceWidth - layout.width);
  layout.y = anchorY + layout.height <= surfaceHeight
                 ? anchorY
                 : std::max(0, anchorY - layout.height + 1);

  layout.items.reserve(static_cast<size_t>(visibleRows));
  for (int i = 0; i < visibleRows; ++i) {
    const ContextMenuItem& item =
        menu.items[static_cast<size_t>(firstVisible + i)];
    ContextMenuCellItem placed;
    placed.token = item.token;
    placed.text = item.label;
    placed.x = layout.x + 1;
    placed.y = layout.y + 1 + i;
    placed.width = innerWidth;
    placed.selected = firstVisible + i == selectedIndex;
    layout.items.push_back(std::move(placed));
  }
  return layout;
}

InteractionMap buildContextMenuInteractionMap(
    const ContextMenuCellLayout& layout) {
  InteractionMap map;
  if (!layout.drawable()) return map;
  map.modal = true;
  map.contextMenuItems.reserve(layout.items.size());
  for (const ContextMenuCellItem& item : layout.items) {
    map.contextMenuItems.push_back(
        {{static_cast<double>(item.x), static_cast<double>(item.y),
          static_cast<double>(item.x + item.width),
          static_cast<double>(item.y + 1)},
         item.token});
  }
  return map;
}

}  // namespace playback_overlay
