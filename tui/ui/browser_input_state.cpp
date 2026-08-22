#include "ui_inputlogic.h"

#include <utility>

#include "browser_navigation.h"

void browser_input::EntryClickTracker::recordPress(
    const BrowserEntry& entry) {
  anchor_ = browserEntryIdentity(entry);
}

std::optional<BrowserState::EntryIdentity>
browser_input::EntryClickTracker::consumeDoubleClickAnchor() {
  std::optional<BrowserState::EntryIdentity> anchor = std::move(anchor_);
  anchor_.reset();
  return anchor;
}

void browser_input::EntryClickTracker::reset() { anchor_.reset(); }

std::optional<ActionStripItem> BrowserPointerState::releaseAction(
    std::optional<ActionStripItem> releasedOver) {
  const std::optional<ActionStripItem> pressed = pressedAction_;
  pressedAction_.reset();
  if (pressed && releasedOver && *pressed == *releasedOver) {
    return pressed;
  }
  return std::nullopt;
}

bool setBrowserHoveredEntry(BrowserState& browser, int entryIndex) {
  if (entryIndex < 0 ||
      entryIndex >= static_cast<int>(browser.entries.size()) ||
      !browser.entries[static_cast<std::size_t>(entryIndex)].isSelectable()) {
    entryIndex = -1;
  }
  if (browser.hovered == entryIndex) {
    return false;
  }
  browser.hovered = entryIndex;
  return true;
}
