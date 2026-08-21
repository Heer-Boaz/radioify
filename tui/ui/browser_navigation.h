#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "browser_model.h"

BrowserState::EntryIdentity browserEntryIdentity(const BrowserEntry& entry);
bool browserEntryMatchesIdentity(
    const BrowserEntry& entry,
    const BrowserState::EntryIdentity& identity);
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
  std::vector<BrowserEntry> entries;
  BrowserContent content;
  int selected = 0;
  int scrollRow = 0;
  BrowserState::ViewportRestoreMode viewportRestoreMode =
      BrowserState::ViewportRestoreMode::None;
  int viewportRestoreScrollRow = 0;
};

using BrowserPreparationId = std::uint64_t;

struct BrowserContentPreparation {
  enum class State {
    Pending,
    Complete,
  };

  static BrowserContentPreparation pending();
  static BrowserContentPreparation complete(
      std::optional<PreparedBrowserContent> content);

  State state = State::Pending;
  std::optional<PreparedBrowserContent> content;
};

class BrowserNavigator {
 public:
  struct Callbacks {
    std::function<BrowserContentPreparation(
        BrowserPreparationId, const BrowserContentRequest&)>
        prepare;
    std::function<void(BrowserPreparationId)> cancelPreparation;
    std::function<void()> changed;
  };

  BrowserNavigator(BrowserState& browser, Callbacks callbacks);

  BrowserState& state() { return browser_; }
  const BrowserState& state() const { return browser_; }

  bool navigate(const BrowserLocation& target,
                const std::string& initialName = {},
                const std::optional<BrowserState::EntryIdentity>& selection =
                    std::nullopt);
  bool reveal(const BrowserLocation& target, const std::string& initialName,
              const BrowserState::EntryIdentity& selection);
  bool restore(const BrowserState::Location& location);
  bool back();
  bool forward();
  bool closeContext();
  bool contextActive() const;
  bool reload(const std::string& initialName = {});
  bool completePreparation(
      BrowserPreparationId preparationId,
      std::optional<PreparedBrowserContent> prepared);
  bool cancelPreparation();

 private:
  using CommitPrepared =
      std::function<void(PreparedBrowserContent prepared)>;

  bool prepare(
      const BrowserLocation& target, const std::string& initialName,
      const std::string& filter, int selected, CommitPrepared commitPrepared);
  void commit(const BrowserLocation& target,
              PreparedBrowserContent prepared,
              bool resetSearch);
  bool activate(const BrowserLocation& target,
                const std::string& initialName,
                const std::optional<BrowserState::EntryIdentity>& selection,
                bool resetSearch, std::function<void()> committed = {});
  bool beginContext(
      const BrowserLocation& target, const std::string& initialName,
      const std::optional<BrowserState::EntryIdentity>& selection);
  bool navigateFromContext(
      const BrowserLocation& target, const std::string& initialName,
      const std::optional<BrowserState::EntryIdentity>& selection);
  bool traverseHistory(bool contextual, bool backward);
  bool restoreLocation(const BrowserState::Location& location,
                       std::function<void()> committed = {});
  void notifyChanged();
  BrowserPreparationId allocatePreparationId();

  BrowserState& browser_;
  Callbacks callbacks_;
  BrowserPreparationId nextPreparationId_ = 1;
  std::optional<BrowserPreparationId> pendingPreparationId_;
  CommitPrepared pendingCommit_;
};

void requestBrowserSelectionReveal(BrowserState& browser);
void applyBrowserViewportRestore(BrowserState& browser,
                                 const GridLayout& layout);
void ensureBrowserSelectionVisible(BrowserState& browser,
                                   const GridLayout& layout);

std::optional<std::filesystem::path> browserParentDirectory(
    const std::filesystem::path& dir);
