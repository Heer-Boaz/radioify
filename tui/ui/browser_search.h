#pragma once

#include <cstdint>
#include <filesystem>

#include "browser_model.h"
#include "input_event.h"

inline bool browserSearchFocused(const BrowserState& browser) {
  return browser.searchFocus != BrowserSearchFocus::None;
}

inline bool browserFilterFocused(const BrowserState& browser) {
  return browser.searchFocus == BrowserSearchFocus::Filter;
}

inline bool browserPathSearchFocused(const BrowserState& browser) {
  return browser.searchFocus == BrowserSearchFocus::PathSearch;
}

// Changes the single browser-search focus owner. Leaving path search clears
// its transient query; the persistent file filter remains intact.
bool setBrowserSearchFocus(BrowserState& browser, BrowserSearchFocus focus);

enum class BrowserSearchEffect : std::uint8_t {
  None,
  Reload,
  Navigate,
};

struct BrowserSearchUpdate {
  bool handled = false;
  bool changed = false;
  BrowserSearchEffect effect = BrowserSearchEffect::None;
  std::filesystem::path navigationTarget;
};

// These functions own the complete lifecycle of the browser's two focused
// text fields. The input router applies typed Reload/Navigate effects; it does
// not edit search state or interpret paths itself.
bool beginBrowserPathSearch(BrowserState& browser);
bool beginBrowserFilter(BrowserState& browser);
BrowserSearchUpdate handleBrowserSearchKey(BrowserState& browser,
                                           const KeyEvent& key);
BrowserSearchUpdate blurBrowserSearch(BrowserState& browser);
bool completeBrowserPathNavigation(BrowserState& browser);
