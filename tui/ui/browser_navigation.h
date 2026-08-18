#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "browser_model.h"

BrowserState::EntryIdentity browserEntryIdentity(const FileEntry& entry);
BrowserState::Location captureBrowserLocation(const BrowserState& browser);

bool selectBrowserEntry(BrowserState& browser,
                        const BrowserState::EntryIdentity& identity);
bool restoreBrowserLocation(BrowserState& browser,
                            const BrowserState::Location& location);
bool recordBrowserNavigation(BrowserState& browser,
                             const BrowserState::Location& from,
                             const BrowserState::Location& to);

struct BrowserContentRequest {
  BrowserLocation location;
  BrowserContent previousContent;
  std::string initialName;
  std::string filter;
  int selected = 0;
  BrowserState::SortMode sortMode = BrowserState::SortMode::Name;
  bool sortDescending = false;
};

struct PreparedBrowserContent {
  std::vector<FileEntry> entries;
  BrowserContent content;
  int selected = 0;
  int scrollRow = 0;
  BrowserState::ViewportRestoreMode viewportRestoreMode =
      BrowserState::ViewportRestoreMode::None;
  int viewportRestoreScrollRow = 0;
};

class BrowserNavigator {
 public:
  struct Callbacks {
    std::function<std::optional<PreparedBrowserContent>(
        const BrowserContentRequest&)>
        prepare;
    std::function<void()> changed;
  };

  BrowserNavigator(BrowserState& browser, Callbacks callbacks);

  BrowserState& state() { return browser_; }
  const BrowserState& state() const { return browser_; }

  bool navigate(const BrowserLocation& target,
                const std::string& initialName = {},
                const std::optional<BrowserState::EntryIdentity>& selection =
                    std::nullopt);
  bool restore(const BrowserState::Location& location);
  bool back();
  bool forward();
  bool closeContext();
  bool contextActive() const;
  bool reload(const std::string& initialName = {});

 private:
  std::optional<PreparedBrowserContent> prepare(
      const BrowserLocation& target, const std::string& initialName,
      const std::string& filter, int selected) const;
  void commit(const BrowserLocation& target,
              PreparedBrowserContent prepared,
              bool resetSearch);
  bool activate(const BrowserLocation& target,
                const std::string& initialName,
                const std::optional<BrowserState::EntryIdentity>& selection);
  bool beginContext(
      const BrowserLocation& target, const std::string& initialName,
      const std::optional<BrowserState::EntryIdentity>& selection);
  bool navigateFromContext(
      const BrowserLocation& target, const std::string& initialName,
      const std::optional<BrowserState::EntryIdentity>& selection);
  bool traverseHistory(
      std::vector<BrowserState::NavigationHistoryEntry>& source,
      std::vector<BrowserState::NavigationHistoryEntry>& destination,
      bool backward);
  bool restoreLocation(const BrowserState::Location& location);
  void notifyChanged();

  BrowserState& browser_;
  Callbacks callbacks_;
};

void requestBrowserSelectionReveal(BrowserState& browser);
void applyBrowserViewportRestore(BrowserState& browser,
                                 const GridLayout& layout);
void ensureBrowserSelectionVisible(BrowserState& browser,
                                   const GridLayout& layout);

std::optional<std::filesystem::path> browserParentDirectory(
    const std::filesystem::path& dir);
