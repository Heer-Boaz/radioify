#pragma once

#include <string>
#include <vector>

#include "playback/overlay/interaction.h"

namespace playback_overlay {

struct ContextMenuItem {
  OverlayControlId control = OverlayControlId::EditOpen;
  std::string label;
};

struct ContextMenuSnapshot {
  bool visible = false;
  double anchorXRatio = 0.5;
  double anchorYRatio = 0.5;
  int selectedControlToken = -1;
  std::vector<ContextMenuItem> items;
};

struct ContextMenuCellItem {
  OverlayControlId control = OverlayControlId::EditOpen;
  std::string text;
  int x = 0;
  int y = 0;
  int width = 0;
  bool selected = false;
};

struct ContextMenuCellLayout {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  std::vector<ContextMenuCellItem> items;

  bool drawable() const {
    return width >= 3 && height >= 3 && !items.empty();
  }
};

ContextMenuCellLayout layoutContextMenuCells(
    const ContextMenuSnapshot& menu, int surfaceWidth, int surfaceHeight);

InteractionMap buildContextMenuInteractionMap(
    const ContextMenuCellLayout& layout);

}  // namespace playback_overlay
