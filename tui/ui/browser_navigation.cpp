#include "browser_navigation.h"

#include <algorithm>
#include <exception>
#include <type_traits>
#include <utility>

#include "browser_grid_index.h"

namespace {

const PathIdentity& cachedPathIdentity(const BrowserEntry& entry,
                                       PathIdentity& fallback) {
  if (!entry.pathIdentity.empty() || entry.path.empty()) {
    return entry.pathIdentity;
  }
  fallback = makePathIdentity(entry.path);
  return fallback;
}

bool actionsMatch(const browser_entry::Action& left,
                  const browser_entry::Action& right) {
  if (left.index() != right.index()) {
    return false;
  }
  return std::visit(
      [&](const auto& action) {
        using T = std::decay_t<decltype(action)>;
        const T* other = std::get_if<T>(&right);
        if (!other) return false;
        if constexpr (std::is_same_v<T, browser_entry::OpenLocation>) {
          return action.target == other->target;
        } else if constexpr (std::is_same_v<T, browser_entry::PlayTrack>) {
          return action.trackIndex == other->trackIndex;
        } else if constexpr (
            std::is_same_v<T, browser_entry::AdjustKssOption> ||
            std::is_same_v<T, browser_entry::AdjustNsfOption> ||
            std::is_same_v<T, browser_entry::AdjustVgmOption> ||
            std::is_same_v<T, browser_entry::AdjustVgmDeviceOption>) {
          return action.option == other->option;
        } else if constexpr (
            std::is_same_v<T, browser_entry::StartInstrumentAudition>) {
          return action.profileIndex == other->profileIndex;
        } else {
          return true;
        }
      },
      left);
}

bool actionUsesNameForIdentity(const browser_entry::Action& action) {
  return std::holds_alternative<browser_entry::Information>(action) ||
         std::holds_alternative<browser_entry::Status>(action) ||
         std::holds_alternative<browser_entry::SectionHeader>(action);
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

BrowserState::EntryIdentity browserEntryIdentity(const BrowserEntry& entry) {
  BrowserState::EntryIdentity identity;
  identity.path = entry.path;
  identity.pathIdentity = entry.pathIdentity.empty() && !entry.path.empty()
                              ? makePathIdentity(entry.path)
                              : entry.pathIdentity;
  identity.name = entry.name;
  identity.action = entry.action;
  return identity;
}

bool browserEntryMatchesIdentity(
    const BrowserEntry& entry,
    const BrowserState::EntryIdentity& identity) {
  PathIdentity fallback;
  if (!entry.isSelectable() ||
      entry.path.empty() != identity.path.empty() ||
      cachedPathIdentity(entry, fallback) != identity.pathIdentity ||
      !actionsMatch(entry.action, identity.action)) {
    return false;
  }
  return !entry.path.empty() || !actionUsesNameForIdentity(entry.action) ||
         entry.name == identity.name;
}

BrowserState::Location captureBrowserLocation(const BrowserState& browser) {
  BrowserState::Location location;
  location.route = browser.location;
  location.scrollRow = browser.scrollRow;
  if (!browser.entries.empty() && browser.selected >= 0 &&
      browser.selected < static_cast<int>(browser.entries.size()) &&
      browser.entries[static_cast<size_t>(browser.selected)].isSelectable()) {
    location.selectedEntry = browserEntryIdentity(
        browser.entries[static_cast<size_t>(browser.selected)]);
  }
  return location;
}

bool selectBrowserEntry(BrowserState& browser,
                        const BrowserState::EntryIdentity& identity) {
  for (size_t i = 0; i < browser.entries.size(); ++i) {
    if (browserEntryMatchesIdentity(browser.entries[i], identity)) {
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

BrowserContentPreparation BrowserContentPreparation::pending() {
  return {BrowserPreparationPending{}};
}

BrowserContentPreparation BrowserContentPreparation::complete(
    PreparedBrowserContent content) {
  return {std::move(content)};
}

BrowserContentPreparation BrowserContentPreparation::failed(
    BrowserPreparationError error) {
  return {std::move(error)};
}

bool BrowserNavigator::prepare(
    const BrowserLocation& target, const std::string& initialName,
    const std::string& filter, int selected,
    CommitPrepared commitPrepared) {
  if (!callbacks_.prepare) {
    browser_.contentError = "Browser preparation service is unavailable.";
    notifyChanged();
    return false;
  }

  BrowserContentRequest request;
  request.location = target;
  request.previousContent = browser_.content;
  request.initialName = initialName;
  request.filter = filter;
  request.selected = selected;
  request.sortMode = browser_.sortMode;
  request.sortDescending = browser_.sortDescending;

  const BrowserPreparationId preparationId = allocatePreparationId();
  if (pendingPreparationId_ && callbacks_.cancelPreparation) {
    callbacks_.cancelPreparation(preparationId);
  }
  pendingPreparationId_ = preparationId;
  pendingCommit_ = std::move(commitPrepared);
  browser_.contentError.clear();

  BrowserContentPreparation preparation;
  try {
    preparation = callbacks_.prepare(preparationId, request);
  } catch (const std::exception& error) {
    return completePreparation(
        preparationId,
        BrowserPreparationError{BrowserPreparationErrorKind::Internal,
                                target, error.what()});
  } catch (...) {
    return completePreparation(
        preparationId,
        BrowserPreparationError{BrowserPreparationErrorKind::Internal,
                                target,
                                "Unexpected browser preparation failure."});
  }

  if (std::holds_alternative<BrowserPreparationPending>(preparation.result)) {
    browser_.contentLoading = true;
    notifyChanged();
    return true;
  }
  if (auto* prepared =
          std::get_if<PreparedBrowserContent>(&preparation.result)) {
    return completePreparation(preparationId, std::move(*prepared));
  }
  return completePreparation(
      preparationId,
      std::move(std::get<BrowserPreparationError>(preparation.result)));
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
  browser_.hovered = -1;
  browser_.scrollRow = prepared.scrollRow;
  browser_.viewportRestoreMode = prepared.viewportRestoreMode;
  browser_.viewportRestoreScrollRow = prepared.viewportRestoreScrollRow;
  browser_.contentError.clear();
}

bool BrowserNavigator::activate(const BrowserLocation& target,
                                const std::string& initialName,
                                const std::optional<BrowserState::EntryIdentity>&
                                    selection,
                                bool resetSearch,
                                std::function<void()> committed) {
  const std::string filter = resetSearch ? std::string{} : browser_.filter;
  const int selected = resetSearch ? 0 : browser_.selected;
  return prepare(
      target, initialName, filter, selected,
      [this, target, selection, resetSearch,
       committed = std::move(committed)](
          PreparedBrowserContent prepared) mutable {
        commit(target, std::move(prepared), resetSearch);
        if (selection && selectBrowserEntry(browser_, *selection)) {
          requestBrowserSelectionReveal(browser_);
        }
        if (committed) {
          committed();
        }
        notifyChanged();
      });
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
  return activate(target, initialName, selection, true,
                  [this, target, origin]() {
                    BrowserState::NavigationContext context;
                    context.kind = target.kind();
                    context.origin = origin;
                    browser_.navigationContext = std::move(context);
                  });
}

bool BrowserNavigator::navigateFromContext(
    const BrowserLocation& target, const std::string& initialName,
    const std::optional<BrowserState::EntryIdentity>& selection) {
  if (!browser_.navigationContext) {
    return false;
  }

  if (target.kind() == browser_.navigationContext->kind) {
    const BrowserState::Location from = captureBrowserLocation(browser_);
    return activate(target, initialName, selection, true, [this, from]() {
      BrowserState::NavigationContext& context = *browser_.navigationContext;
      recordNavigation(context.backHistory, context.forwardHistory, from,
                       captureBrowserLocation(browser_));
    });
  }

  const BrowserState::Location origin = browser_.navigationContext->origin;
  return activate(
      target, initialName, selection, true,
      [this, origin, initialName, selection]() {
        if (browser_.location == origin.route && initialName.empty() &&
            !selection) {
          restoreBrowserLocation(browser_, origin);
        }
        browser_.navigationContext.reset();
        recordBrowserNavigation(browser_, origin,
                                captureBrowserLocation(browser_));
      });
}

bool BrowserNavigator::navigate(const BrowserLocation& target,
                                const std::string& initialName,
                                const std::optional<BrowserState::EntryIdentity>&
                                    selection) {
  if (target == browser_.location) {
    return activate(target, initialName, selection, false);
  }
  if (browser_.navigationContext) {
    return navigateFromContext(target, initialName, selection);
  }
  if (browserLocationIsContextual(target) &&
      !browserLocationIsContextual(browser_.location)) {
    return beginContext(target, initialName, selection);
  }
  const BrowserState::Location from = captureBrowserLocation(browser_);
  return activate(target, initialName, selection, true, [this, from]() {
    recordBrowserNavigation(browser_, from, captureBrowserLocation(browser_));
  });
}

bool BrowserNavigator::initialize(const BrowserLocation& target,
                                  const std::string& initialName) {
  if (!browser_.entries.empty() || browser_.navigationContext ||
      !browser_.backHistory.empty() || !browser_.forwardHistory.empty()) {
    return false;
  }
  return activate(target, initialName, std::nullopt, false);
}

bool BrowserNavigator::reveal(
    const BrowserLocation& target, const std::string& initialName,
    const BrowserState::EntryIdentity& selection) {
  if (target != browser_.location) {
    return navigate(target, initialName, selection);
  }
  return activate(target, initialName, selection, true);
}

bool BrowserNavigator::restoreLocation(
    const BrowserState::Location& location, std::function<void()> committed) {
  return activate(
      location.route, {}, std::nullopt, true,
      [this, location, committed = std::move(committed)]() mutable {
        restoreBrowserLocation(browser_, location);
        if (committed) {
          committed();
        }
      });
}

bool BrowserNavigator::restore(const BrowserState::Location& location) {
  return restoreLocation(location, [this, location]() {
    if (browser_.navigationContext &&
        location.route.kind() != browser_.navigationContext->kind) {
      browser_.navigationContext.reset();
    }
  });
}

bool BrowserNavigator::traverseHistory(bool contextual, bool backward) {
  auto* source = contextual ? &browser_.navigationContext->backHistory
                            : &browser_.backHistory;
  if (!backward) {
    source = contextual ? &browser_.navigationContext->forwardHistory
                        : &browser_.forwardHistory;
  }
  if (source->empty()) {
    return false;
  }

  BrowserState::NavigationHistoryEntry entry = source->back();
  const BrowserState::Location current = captureBrowserLocation(browser_);
  const BrowserState::Location target = backward ? entry.from : entry.to;
  return restoreLocation(
      target, [this, contextual, backward, entry = std::move(entry),
               current]() mutable {
        auto* committedSource =
            contextual ? &browser_.navigationContext->backHistory
                       : &browser_.backHistory;
        auto* committedDestination =
            contextual ? &browser_.navigationContext->forwardHistory
                       : &browser_.forwardHistory;
        if (!backward) {
          std::swap(committedSource, committedDestination);
        }
        committedSource->pop_back();
        if (backward) {
          entry.to = current;
        } else {
          entry.from = current;
        }
        committedDestination->push_back(std::move(entry));
      });
}

bool BrowserNavigator::back() {
  if (pendingPreparationId_) {
    return cancelPreparation();
  }
  if (browser_.navigationContext) {
    BrowserState::NavigationContext& context = *browser_.navigationContext;
    if (context.backHistory.empty()) {
      return closeContext();
    }
    return traverseHistory(true, true);
  }
  return traverseHistory(false, true);
}

bool BrowserNavigator::forward() {
  if (browser_.navigationContext) {
    return traverseHistory(true, false);
  }
  return traverseHistory(false, false);
}

bool BrowserNavigator::closeContext() {
  if (!browser_.navigationContext) {
    return false;
  }

  const BrowserState::Location origin = browser_.navigationContext->origin;
  return restoreLocation(origin, [this]() { browser_.navigationContext.reset(); });
}

bool BrowserNavigator::contextActive() const {
  return browser_.navigationContext.has_value();
}

bool BrowserNavigator::reload(const std::string& initialName) {
  return activate(browser_.location, initialName, std::nullopt, false);
}

bool BrowserNavigator::completePreparation(
    BrowserPreparationId preparationId,
    BrowserPreparationResult result) {
  if (!pendingPreparationId_ || *pendingPreparationId_ != preparationId ||
      !pendingCommit_) {
    return false;
  }

  CommitPrepared commitPrepared = std::move(pendingCommit_);
  pendingCommit_ = {};
  pendingPreparationId_.reset();
  browser_.contentLoading = false;
  if (auto* error = std::get_if<BrowserPreparationError>(&result)) {
    browser_.contentError = error->message;
    if (callbacks_.failed) {
      callbacks_.failed(*error);
    }
    notifyChanged();
    return false;
  }

  commitPrepared(std::move(std::get<PreparedBrowserContent>(result)));
  return true;
}

bool BrowserNavigator::cancelPreparation() {
  if (!pendingPreparationId_ || !pendingCommit_) {
    return false;
  }

  pendingCommit_ = {};
  pendingPreparationId_.reset();
  browser_.contentLoading = false;
  const BrowserPreparationId cancellationId = allocatePreparationId();
  if (callbacks_.cancelPreparation) {
    callbacks_.cancelPreparation(cancellationId);
  }
  notifyChanged();
  return true;
}

void BrowserNavigator::notifyChanged() {
  if (callbacks_.changed) {
    callbacks_.changed();
  }
}

BrowserPreparationId BrowserNavigator::allocatePreparationId() {
  return nextPreparationId_++;
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
