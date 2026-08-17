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

class BrowserNavigator {
 public:
  struct Callbacks {
    std::function<bool(const BrowserLocation&)> activate;
    std::function<void(const std::string&)> refresh;
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
  void reload(const std::string& initialName = {});

 private:
  bool activate(const BrowserLocation& target,
                const std::string& initialName,
                const std::optional<BrowserState::EntryIdentity>& selection);
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
