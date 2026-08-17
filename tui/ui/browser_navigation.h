#pragma once

#include <filesystem>
#include <optional>

#include "browser_model.h"

BrowserState::EntryIdentity browserEntryIdentity(const FileEntry& entry);
BrowserState::Location captureBrowserLocation(const BrowserState& browser,
                                              BrowserState::LocationKind kind);

bool selectBrowserEntry(BrowserState& browser,
                        const BrowserState::EntryIdentity& identity);
bool restoreBrowserLocation(BrowserState& browser,
                            const BrowserState::Location& location);
bool recordBrowserNavigation(BrowserState& browser,
                             const BrowserState::Location& from,
                             const BrowserState::Location& to);
std::optional<BrowserState::Location> browserHistoryBack(
    BrowserState& browser, const BrowserState::Location& current);
std::optional<BrowserState::Location> browserHistoryForward(
    BrowserState& browser, const BrowserState::Location& current);

void requestBrowserSelectionReveal(BrowserState& browser);
void applyBrowserViewportRestore(BrowserState& browser,
                                 const GridLayout& layout);
void ensureBrowserSelectionVisible(BrowserState& browser,
                                   const GridLayout& layout);

std::optional<std::filesystem::path> browserParentDirectory(
    const std::filesystem::path& dir);
