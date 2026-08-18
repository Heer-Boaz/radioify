#include "browser_navigation.h"

#include <algorithm>
#include <utility>

#include "browser_grid_index.h"

namespace {

const PathIdentity& cachedPathIdentity(const FileEntry& entry,
                                       PathIdentity& fallback) {
  if (!entry.pathIdentity.empty() || entry.path.empty()) {
    return entry.pathIdentity;
  }
  fallback = makePathIdentity(entry.path);
  return fallback;
}

bool matchesIdentity(const FileEntry& entry,
                     const BrowserState::EntryIdentity& identity) {
  PathIdentity fallback;
  if (entry.isSectionHeader || entry.isDir != identity.isDir ||
      entry.trackIndex != identity.trackIndex ||
      entry.path.empty() != identity.path.empty() ||
      cachedPathIdentity(entry, fallback) != identity.pathIdentity) {
    return false;
  }
  return !entry.path.empty() || entry.name == identity.name;
}

int rowFromIndex(int idx, const GridLayout& layout) {
  return idx / std::max(1, layout.cols);
}

bool recordNavigation(
    std::vector<BrowserState::NavigationHistoryEntry>& backHistory,
    std::vector<BrowserState::NavigationHistoryEntry>& forwardHistory,
    const BrowserState::Location& from, const BrowserState::Location& to) {
  if (from.route == to.route) {
    return false;
  }

  backHistory.push_back({from, to});
  forwardHistory.clear();
  return true;
}

}  // namespace

BrowserState::EntryIdentity browserEntryIdentity(const FileEntry& entry) {
  BrowserState::EntryIdentity identity;
  identity.path = entry.path;
  identity.pathIdentity = entry.pathIdentity.empty() && !entry.path.empty()
                              ? makePathIdentity(entry.path)
                              : entry.pathIdentity;
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
  return recordNavigation(browser.backHistory, browser.forwardHistory, from,
                          to);
}

BrowserNavigator::BrowserNavigator(BrowserState& browser, Callbacks callbacks)
    : browser_(browser), callbacks_(std::move(callbacks)) {}

std::optional<PreparedBrowserContent> BrowserNavigator::prepare(
    const BrowserLocation& target, const std::string& initialName,
    const std::string& filter, int selected) const {
  if (!callbacks_.prepare) {
    return std::nullopt;
  }
  BrowserContentRequest request;
  request.location = target;
  request.previousContent = browser_.content;
  request.initialName = initialName;
  request.filter = filter;
  request.selected = selected;
  request.sortMode = browser_.sortMode;
  request.sortDescending = browser_.sortDescending;
  return callbacks_.prepare(request);
}

void BrowserNavigator::commit(const BrowserLocation& target,
                              PreparedBrowserContent prepared,
                              bool resetSearch) {
  browser_.location = target;
  if (resetSearch) {
    browser_.filter.clear();
    browser_.filterActive = false;
    browser_.pathSearch.clear();
    browser_.pathSearchActive = false;
  }
  browser_.entries = std::move(prepared.entries);
  browser_.content = std::move(prepared.content);
  browser_.selected = prepared.selected;
  browser_.scrollRow = prepared.scrollRow;
  browser_.viewportRestoreMode = prepared.viewportRestoreMode;
  browser_.viewportRestoreScrollRow = prepared.viewportRestoreScrollRow;
}

bool BrowserNavigator::activate(const BrowserLocation& target,
                                const std::string& initialName,
                                const std::optional<BrowserState::EntryIdentity>&
                                    selection) {
  std::optional<PreparedBrowserContent> prepared =
      prepare(target, initialName, {}, 0);
  if (!prepared) {
    return false;
  }

  commit(target, std::move(*prepared), true);
  if (selection && selectBrowserEntry(browser_, *selection)) {
    requestBrowserSelectionReveal(browser_);
  }
  return true;
}

bool BrowserNavigator::beginContext(
    const BrowserLocation& target, const std::string& initialName,
    const std::optional<BrowserState::EntryIdentity>& selection) {
  if (browser_.navigationContext ||
      !browserLocationIsContextual(target) ||
      browserLocationIsContextual(browser_.location)) {
    return false;
  }

  const BrowserState::Location origin = captureBrowserLocation(browser_);
  if (!activate(target, initialName, selection)) {
    return false;
  }

  BrowserState::NavigationContext context;
  context.kind = target.kind;
  context.origin = origin;
  browser_.navigationContext = std::move(context);
  notifyChanged();
  return true;
}

bool BrowserNavigator::navigateFromContext(
    const BrowserLocation& target, const std::string& initialName,
    const std::optional<BrowserState::EntryIdentity>& selection) {
  if (!browser_.navigationContext) {
    return false;
  }

  if (target.kind == browser_.navigationContext->kind) {
    const BrowserState::Location from = captureBrowserLocation(browser_);
    if (!activate(target, initialName, selection)) {
      return false;
    }
    BrowserState::NavigationContext& context = *browser_.navigationContext;
    recordNavigation(context.backHistory, context.forwardHistory, from,
                     captureBrowserLocation(browser_));
    notifyChanged();
    return true;
  }

  const BrowserState::Location origin = browser_.navigationContext->origin;
  if (!activate(target, initialName, selection)) {
    return false;
  }
  if (browser_.location == origin.route && initialName.empty() && !selection) {
    restoreBrowserLocation(browser_, origin);
  }
  browser_.navigationContext.reset();
  recordBrowserNavigation(browser_, origin, captureBrowserLocation(browser_));
  notifyChanged();
  return true;
}

bool BrowserNavigator::navigate(const BrowserLocation& target,
                                const std::string& initialName,
                                const std::optional<BrowserState::EntryIdentity>&
                                    selection) {
  if (target == browser_.location) {
    std::optional<PreparedBrowserContent> prepared = prepare(
        target, initialName, browser_.filter, browser_.selected);
    if (!prepared) {
      return false;
    }
    commit(target, std::move(*prepared), false);
    if (selection && selectBrowserEntry(browser_, *selection)) {
      requestBrowserSelectionReveal(browser_);
    }
    notifyChanged();
    return true;
  }
  if (browser_.navigationContext) {
    return navigateFromContext(target, initialName, selection);
  }
  if (browserLocationIsContextual(target) &&
      !browserLocationIsContextual(browser_.location)) {
    return beginContext(target, initialName, selection);
  }
  const BrowserState::Location from = captureBrowserLocation(browser_);
  if (!activate(target, initialName, selection)) {
    return false;
  }
  recordBrowserNavigation(browser_, from, captureBrowserLocation(browser_));
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
  if (browser_.navigationContext &&
      location.route.kind != browser_.navigationContext->kind) {
    browser_.navigationContext.reset();
  }
  notifyChanged();
  return true;
}

bool BrowserNavigator::traverseHistory(
    std::vector<BrowserState::NavigationHistoryEntry>& source,
    std::vector<BrowserState::NavigationHistoryEntry>& destination,
    bool backward) {
  if (source.empty()) {
    return false;
  }

  BrowserState::NavigationHistoryEntry entry = source.back();
  const BrowserState::Location current = captureBrowserLocation(browser_);
  const BrowserState::Location& target = backward ? entry.from : entry.to;
  if (!restoreLocation(target)) {
    return false;
  }

  source.pop_back();
  if (backward) {
    entry.to = current;
  } else {
    entry.from = current;
  }
  destination.push_back(std::move(entry));
  notifyChanged();
  return true;
}

bool BrowserNavigator::back() {
  if (browser_.navigationContext) {
    BrowserState::NavigationContext& context = *browser_.navigationContext;
    if (context.backHistory.empty()) {
      return closeContext();
    }
    return traverseHistory(context.backHistory, context.forwardHistory, true);
  }
  return traverseHistory(browser_.backHistory, browser_.forwardHistory, true);
}

bool BrowserNavigator::forward() {
  if (browser_.navigationContext) {
    BrowserState::NavigationContext& context = *browser_.navigationContext;
    return traverseHistory(context.forwardHistory, context.backHistory, false);
  }
  return traverseHistory(browser_.forwardHistory, browser_.backHistory, false);
}

bool BrowserNavigator::closeContext() {
  if (!browser_.navigationContext) {
    return false;
  }

  const BrowserState::Location origin = browser_.navigationContext->origin;
  if (!restoreLocation(origin)) {
    return false;
  }
  browser_.navigationContext.reset();
  notifyChanged();
  return true;
}

bool BrowserNavigator::contextActive() const {
  return browser_.navigationContext.has_value();
}

bool BrowserNavigator::reload(const std::string& initialName) {
  std::optional<PreparedBrowserContent> prepared = prepare(
      browser_.location, initialName, browser_.filter, browser_.selected);
  if (!prepared) {
    return false;
  }
  commit(browser_.location, std::move(*prepared), false);
  notifyChanged();
  return true;
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
