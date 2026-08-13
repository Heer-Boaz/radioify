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

  const int visibleRows =
      std::min(static_cast<int>(menu.items.size()), surfaceHeight - 2);
  int labelWidth = 1;
  for (int i = 0; i < visibleRows; ++i) {
    labelWidth = std::max(
        labelWidth,
        utf8DisplayWidth(menu.items[static_cast<size_t>(i)].label));
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
  bool hasSelectedItem = false;
  for (int i = 0; i < visibleRows; ++i) {
    const ContextMenuItem& item = menu.items[static_cast<size_t>(i)];
    ContextMenuCellItem placed;
    placed.control = item.control;
    placed.text = item.label;
    placed.x = layout.x + 1;
    placed.y = layout.y + 1 + i;
    placed.width = innerWidth;
    placed.selected =
        overlayControlToken(item.control) == menu.selectedControlToken;
    hasSelectedItem = hasSelectedItem || placed.selected;
    layout.items.push_back(std::move(placed));
  }
  if (!hasSelectedItem) layout.items.front().selected = true;
  return layout;
}

InteractionMap buildContextMenuInteractionMap(
    const ContextMenuCellLayout& layout) {
  InteractionMap map;
  if (!layout.drawable()) return map;
  map.modal = true;
  map.controls.reserve(layout.items.size());
  for (const ContextMenuCellItem& item : layout.items) {
    map.controls.push_back(
        {{static_cast<double>(item.x), static_cast<double>(item.y),
          static_cast<double>(item.x + item.width),
          static_cast<double>(item.y + 1)},
         item.control});
  }
  return map;
}

}  // namespace playback_overlay
