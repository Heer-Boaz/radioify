#pragma once

#include "browser_model.h"

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
