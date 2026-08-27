#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "browser_model.h"
#include "optionsbrowser.h"

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
  OptionsBrowserRuntimeSnapshot optionsRuntime;
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

enum class BrowserPreparationErrorKind : std::uint8_t {
  Unavailable,
  Unsupported,
  Internal,
};

struct BrowserPreparationError {
  BrowserPreparationErrorKind kind;
  BrowserLocation location;
  std::string message;
};

using BrowserPreparationResult =
    std::variant<PreparedBrowserContent, BrowserPreparationError>;

struct BrowserPreparationPending {};

struct BrowserContentPreparation {
  static BrowserContentPreparation pending();
  static BrowserContentPreparation complete(PreparedBrowserContent content);
  static BrowserContentPreparation failed(BrowserPreparationError error);

  std::variant<BrowserPreparationPending, PreparedBrowserContent,
               BrowserPreparationError>
      result;
};

class BrowserPreparationService {
 public:
  virtual ~BrowserPreparationService() = default;

  virtual BrowserContentPreparation prepare(
      BrowserPreparationId preparationId,
      const BrowserContentRequest& request) = 0;
  // Advances the worker generation so older work and completions become
  // stale. The supplied generation is intentionally newer than the request
  // being invalidated.
  virtual void cancelThrough(BrowserPreparationId generation) = 0;
};

class BrowserNavigator {
 public:
  struct Changed {};
  struct PreparationFailed {
    BrowserPreparationError error;
  };
  using Event = std::variant<Changed, PreparationFailed>;

  BrowserNavigator(BrowserState& browser,
                   BrowserPreparationService& preparationService);

  BrowserState& state() { return browser_; }
  const BrowserState& state() const { return browser_; }

  bool initialize(const BrowserLocation& target,
                  const std::string& initialName = {});
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
      BrowserPreparationResult result);
  bool cancelPreparation();
  bool select(const BrowserState::EntryIdentity& selection);
  std::optional<BrowserPreparationId> pendingPreparationId() const {
    return pendingPreparationId_;
  }
  std::vector<Event> drainEvents();

 private:
  struct NoCommitEffect {};
  struct BeginContextEffect {
    BrowserLocation target;
    BrowserState::Location origin;
  };
  struct RecordNavigationEffect {
    BrowserState::Location from;
  };
  struct RecordContextNavigationEffect {
    BrowserState::Location from;
  };
  struct LeaveContextEffect {
    BrowserState::Location origin;
    bool restoreOriginViewport = false;
  };
  struct RestoreContextBoundaryEffect {
    BrowserLocation target;
  };
  struct TraverseHistoryEffect {
    bool contextual = false;
    bool backward = false;
    BrowserState::NavigationHistoryEntry entry;
    BrowserState::Location current;
  };
  struct CloseContextEffect {};
  using CommitEffect =
      std::variant<NoCommitEffect, BeginContextEffect,
                   RecordNavigationEffect, RecordContextNavigationEffect,
                   LeaveContextEffect, RestoreContextBoundaryEffect,
                   TraverseHistoryEffect, CloseContextEffect>;

  struct PendingCommit {
    BrowserLocation target;
    std::optional<BrowserState::EntryIdentity> selection;
    bool resetSearch = false;
    std::optional<BrowserState::Location> restoreLocation;
    CommitEffect effect;
  };

  bool prepare(PendingCommit pending, const std::string& initialName);
  void commit(const BrowserLocation& target,
              PreparedBrowserContent prepared,
              bool resetSearch);
  void applyCommitEffect(CommitEffect effect);
  void commitPrepared(PendingCommit pending,
                      PreparedBrowserContent prepared);
  bool activate(const BrowserLocation& target,
                const std::string& initialName,
                const std::optional<BrowserState::EntryIdentity>& selection,
                bool resetSearch,
                CommitEffect effect = NoCommitEffect{});
  bool beginContext(
      const BrowserLocation& target, const std::string& initialName,
      const std::optional<BrowserState::EntryIdentity>& selection);
  bool navigateFromContext(
      const BrowserLocation& target, const std::string& initialName,
      const std::optional<BrowserState::EntryIdentity>& selection);
  bool traverseHistory(bool contextual, bool backward);
  bool restoreLocation(const BrowserState::Location& location,
                       CommitEffect effect = NoCommitEffect{});
  void notifyChanged();
  BrowserPreparationId allocatePreparationId();

  BrowserState& browser_;
  BrowserPreparationService& preparationService_;
  std::vector<Event> events_;
  BrowserPreparationId nextPreparationId_ = 1;
  std::optional<BrowserPreparationId> pendingPreparationId_;
  std::optional<PendingCommit> pendingCommit_;
};

void requestBrowserSelectionReveal(BrowserState& browser);
void applyBrowserViewportRestore(BrowserState& browser,
                                 const GridLayout& layout);
void ensureBrowserSelectionVisible(BrowserState& browser,
                                   const GridLayout& layout);

std::optional<std::filesystem::path> browserParentDirectory(
    const std::filesystem::path& dir);
