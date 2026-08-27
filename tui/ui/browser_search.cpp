#include "browser_search.h"

bool setBrowserSearchFocus(BrowserState& browser, BrowserSearchFocus focus) {
  if (browser.searchFocus == focus) {
    return false;
  }

  const bool leavingPathSearch = browserPathSearchFocused(browser);
  browser.searchFocus = focus;
  if (leavingPathSearch) {
    browser.pathSearch.clear();
  }
  return true;
}
