#include "browser_navigation.h"

#include <algorithm>
#include <utility>

#include "browser_grid_index.h"

namespace {

bool matchesIdentity(const FileEntry& entry,
                     const BrowserState::EntryIdentity& identity) {
  if (entry.isSectionHeader || entry.isDir != identity.isDir ||
      entry.trackIndex != identity.trackIndex || entry.path != identity.path) {
    return false;
  }
  return !entry.path.empty() || entry.name == identity.name;
}

int rowFromIndex(int idx, const GridLayout& layout) {
  return idx / std::max(1, layout.cols);
}

}  // namespace

BrowserState::EntryIdentity browserEntryIdentity(const FileEntry& entry) {
  BrowserState::EntryIdentity identity;
  identity.path = entry.path;
  identity.name = entry.name;
  identity.isDir = entry.isDir;
  identity.trackIndex = entry.trackIndex;
  return identity;
}

BrowserState::Location captureBrowserLocation(const BrowserState& browser) {
  BrowserState::Location location;
  location.route = browser.location;
  location.scrollRow = browser.scrollRow;
  if (!browser.entries.empty() && browser.selected >= 0 &&
      browser.selected < static_cast<int>(browser.entries.size()) &&
      !browser.entries[static_cast<size_t>(browser.selected)].isSectionHeader) {
    location.selectedEntry = browserEntryIdentity(
        browser.entries[static_cast<size_t>(browser.selected)]);
  }
  return location;
}

bool selectBrowserEntry(BrowserState& browser,
                        const BrowserState::EntryIdentity& identity) {
  for (size_t i = 0; i < browser.entries.size(); ++i) {
    if (matchesIdentity(browser.entries[i], identity)) {
      browser.selected = static_cast<int>(i);
      return true;
    }
  }
  return false;
}

bool restoreBrowserLocation(BrowserState& browser,
                            const BrowserState::Location& location) {
  if (browser.location != location.route) {
    return false;
  }

  const bool restoredSelection =
      !location.selectedEntry ||
      selectBrowserEntry(browser, *location.selectedEntry);
  if (restoredSelection) {
    browser.scrollRow = std::max(0, location.scrollRow);
    browser.viewportRestoreMode =
        BrowserState::ViewportRestoreMode::RestoreScroll;
    browser.viewportRestoreScrollRow = browser.scrollRow;
  } else {
    browser.scrollRow = 0;
    requestBrowserSelectionReveal(browser);
  }
  return restoredSelection;
}

bool recordBrowserNavigation(BrowserState& browser,
                             const BrowserState::Location& from,
                             const BrowserState::Location& to) {
  if (from.route == to.route) {
    return false;
  }

  browser.backHistory.push_back({from, to});
  browser.forwardHistory.clear();
  return true;
}

BrowserNavigator::BrowserNavigator(BrowserState& browser, Callbacks callbacks)
    : browser_(browser), callbacks_(std::move(callbacks)) {}

bool BrowserNavigator::activate(const BrowserLocation& target,
                                const std::string& initialName,
                                const std::optional<BrowserState::EntryIdentity>&
                                    selection) {
  if (callbacks_.activate && !callbacks_.activate(target)) {
    return false;
  }

  browser_.location = target;
  browser_.selected = 0;
  browser_.scrollRow = 0;
  browser_.filter.clear();
  browser_.filterActive = false;
  browser_.pathSearch.clear();
  browser_.pathSearchActive = false;
  browser_.viewportRestoreMode = BrowserState::ViewportRestoreMode::None;
  browser_.viewportRestoreScrollRow = 0;
  if (callbacks_.refresh) {
    callbacks_.refresh(initialName);
  }
  if (selection && selectBrowserEntry(browser_, *selection)) {
    requestBrowserSelectionReveal(browser_);
  }
  return true;
}

bool BrowserNavigator::navigate(const BrowserLocation& target,
                                const std::string& initialName,
                                const std::optional<BrowserState::EntryIdentity>&
                                    selection) {
  if (target == browser_.location) {
    if (callbacks_.refresh) {
      callbacks_.refresh(initialName);
    }
    if (selection && selectBrowserEntry(browser_, *selection)) {
      requestBrowserSelectionReveal(browser_);
    }
    notifyChanged();
    return true;
  }
  const BrowserState::Location from = captureBrowserLocation(browser_);
  if (!activate(target, initialName, selection)) {
    return false;
  }
  recordBrowserNavigation(browser_, from, captureBrowserLocation(browser_));
  notifyChanged();
  return true;
}

bool BrowserNavigator::replace(const BrowserLocation& target,
                               const std::string& initialName,
                               const std::optional<BrowserState::EntryIdentity>&
                                   selection) {
  if (target == browser_.location) {
    if (callbacks_.refresh) {
      callbacks_.refresh(initialName);
    }
    if (selection && selectBrowserEntry(browser_, *selection)) {
      requestBrowserSelectionReveal(browser_);
    }
    notifyChanged();
    return true;
  }
  if (!activate(target, initialName, selection)) {
    return false;
  }
  notifyChanged();
  return true;
}

bool BrowserNavigator::restoreLocation(
    const BrowserState::Location& location) {
  if (!activate(location.route, {}, std::nullopt)) {
    return false;
  }
  restoreBrowserLocation(browser_, location);
  return true;
}

bool BrowserNavigator::restore(const BrowserState::Location& location) {
  if (!restoreLocation(location)) {
    return false;
  }
  notifyChanged();
  return true;
}

bool BrowserNavigator::back() {
  if (browser_.backHistory.empty()) {
    return false;
  }

  BrowserState::NavigationHistoryEntry entry = browser_.backHistory.back();
  const BrowserState::Location current = captureBrowserLocation(browser_);
  if (!restoreLocation(entry.from)) {
    return false;
  }

  browser_.backHistory.pop_back();
  entry.to = current;
  browser_.forwardHistory.push_back(std::move(entry));
  notifyChanged();
  return true;
}

bool BrowserNavigator::forward() {
  if (browser_.forwardHistory.empty()) {
    return false;
  }

  BrowserState::NavigationHistoryEntry entry = browser_.forwardHistory.back();
  const BrowserState::Location current = captureBrowserLocation(browser_);
  if (!restoreLocation(entry.to)) {
    return false;
  }

  browser_.forwardHistory.pop_back();
  entry.from = current;
  browser_.backHistory.push_back(std::move(entry));
  notifyChanged();
  return true;
}

void BrowserNavigator::reload(const std::string& initialName) {
  if (callbacks_.refresh) {
    callbacks_.refresh(initialName);
  }
  notifyChanged();
}

void BrowserNavigator::notifyChanged() {
  if (callbacks_.changed) {
    callbacks_.changed();
  }
}

void requestBrowserSelectionReveal(BrowserState& browser) {
  browser.viewportRestoreMode =
      BrowserState::ViewportRestoreMode::RevealSelection;
}

void ensureBrowserSelectionVisible(BrowserState& browser,
                                   const GridLayout& layout) {
  if (browser.entries.empty() || layout.totalRows <= 0) {
    browser.scrollRow = 0;
    return;
  }
  if (layout.totalRows <= layout.rowsVisible) {
    browser.scrollRow = 0;
    return;
  }

  const int maxScroll = std::max(0, layout.totalRows - layout.rowsVisible);
  if (browser.viewMode == BrowserState::ViewMode::ListOnly) {
    const int visibleCapacity = browserGridVisibleCapacity(layout);
    if (visibleCapacity <= 0 || maxScroll <= 0) {
      browser.scrollRow = 0;
      return;
    }
    if (browser.selected < browser.scrollRow) {
      browser.scrollRow = browser.selected;
    } else if (browser.selected >= browser.scrollRow + visibleCapacity) {
      browser.scrollRow = browser.selected - visibleCapacity + 1;
    }
    browser.scrollRow = std::clamp(browser.scrollRow, 0, maxScroll);
    return;
  }

  const int row = rowFromIndex(browser.selected, layout);
  if (row < browser.scrollRow) {
    browser.scrollRow = row;
  } else if (row >= browser.scrollRow + layout.rowsVisible) {
    browser.scrollRow = row - layout.rowsVisible + 1;
  }
  browser.scrollRow = std::clamp(browser.scrollRow, 0, maxScroll);
}

void applyBrowserViewportRestore(BrowserState& browser,
                                 const GridLayout& layout) {
  const int maxScroll = std::max(0, layout.totalRows - layout.rowsVisible);
  if (browser.viewportRestoreMode ==
      BrowserState::ViewportRestoreMode::RestoreScroll) {
    browser.scrollRow =
        std::clamp(browser.viewportRestoreScrollRow, 0, maxScroll);
    ensureBrowserSelectionVisible(browser, layout);
  } else if (browser.viewportRestoreMode ==
             BrowserState::ViewportRestoreMode::RevealSelection) {
    browser.scrollRow = std::clamp(browser.scrollRow, 0, maxScroll);
    ensureBrowserSelectionVisible(browser, layout);
  } else {
    browser.scrollRow = std::clamp(browser.scrollRow, 0, maxScroll);
  }

  browser.viewportRestoreMode = BrowserState::ViewportRestoreMode::None;
  browser.viewportRestoreScrollRow = 0;
}

std::optional<std::filesystem::path> browserParentDirectory(
    const std::filesystem::path& dir) {
  if (dir.empty()) {
    return std::nullopt;
  }
#ifdef _WIN32
  if (dir == dir.root_path()) {
    return std::filesystem::path();
  }
#endif
  if (!dir.has_parent_path()) {
    return std::nullopt;
  }
  const std::filesystem::path parent = dir.parent_path();
  if (parent == dir) {
    return std::nullopt;
  }
  return parent;
}
