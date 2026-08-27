#include "browser_search.h"

#include <cstdlib>
#include <optional>
#include <string>

#include "single_line_text_input.h"

namespace {

std::optional<std::string> environmentValue(const char* name) {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || !value) {
    return std::nullopt;
  }
  std::string result(value);
  std::free(value);
  return result;
#else
  const char* value = std::getenv(name);
  return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

std::optional<std::filesystem::path> resolvePathSearchTarget(
    const BrowserState& browser) {
  if (browser.pathSearch.empty()) return std::nullopt;
  std::filesystem::path target;
  const std::string& query = browser.pathSearch;
  if (query.front() == '~') {
    std::optional<std::string> home = environmentValue("USERPROFILE");
    if (!home || home->empty()) {
      const std::optional<std::string> drive =
          environmentValue("HOMEDRIVE");
      const std::optional<std::string> path = environmentValue("HOMEPATH");
      if (drive && !drive->empty() && path && !path->empty()) {
        home = *drive + *path;
      }
    }
    if (home && !home->empty()) {
      target = std::filesystem::path(*home);
      if (query.size() > 1) {
        const std::size_t relativeStart =
            query[1] == '/' || query[1] == '\\' ? 2u : 1u;
        target /= std::filesystem::path(query.substr(relativeStart));
      }
    } else {
      target = std::filesystem::path(query);
    }
  } else {
    target = std::filesystem::path(query);
  }

  if (target.has_root_name() && !target.has_root_directory() &&
      target.relative_path().empty()) {
    target = target.root_name();
    target /= std::filesystem::path();
  }
  if (!target.is_absolute()) {
    const std::filesystem::path base =
        browser.location.kind() == BrowserLocationKind::Directory
            ? browser.location.path()
            : browser.location.path().parent_path();
    target = base / target;
  }
  if (!target.has_root_name() && !target.has_root_directory() &&
      target.relative_path().empty()) {
    return std::nullopt;
  }
  std::error_code error;
  if (!std::filesystem::is_directory(target, error) || error) {
    return std::nullopt;
  }
  return target;
}

}  // namespace

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

bool beginBrowserPathSearch(BrowserState& browser) {
  bool changed = setBrowserSearchFocus(
      browser, BrowserSearchFocus::PathSearch);
  if (!browser.pathSearch.empty()) {
    browser.pathSearch.clear();
    changed = true;
  }
  return changed;
}

bool beginBrowserFilter(BrowserState& browser) {
  if (!browserFilterFocused(browser)) {
    browser.filterBackup = browser.filter;
  }
  bool changed =
      setBrowserSearchFocus(browser, BrowserSearchFocus::Filter);
  if (!browser.pathSearch.empty()) {
    browser.pathSearch.clear();
    changed = true;
  }
  return changed;
}

BrowserSearchUpdate handleBrowserSearchKey(BrowserState& browser,
                                           const KeyEvent& key) {
  BrowserSearchUpdate update;
  if (!browserSearchFocused(browser)) return update;
  update.handled = true;

  const BrowserSearchFocus focus = browser.searchFocus;
  std::string& text = focus == BrowserSearchFocus::PathSearch
                          ? browser.pathSearch
                          : browser.filter;
  const single_line_text_input::EditResult edit =
      single_line_text_input::edit(text, key);
  update.changed = edit.changed;

  if (edit.intent == single_line_text_input::Intent::Cancel) {
    if (focus == BrowserSearchFocus::Filter) {
      if (browser.filter != browser.filterBackup) {
        browser.filter = browser.filterBackup;
        update.changed = true;
      }
      update.effect = BrowserSearchEffect::Reload;
    }
    update.changed =
        setBrowserSearchFocus(browser, BrowserSearchFocus::None) ||
        update.changed;
    return update;
  }

  if (edit.intent != single_line_text_input::Intent::Commit) return update;
  if (focus == BrowserSearchFocus::Filter) {
    update.changed =
        setBrowserSearchFocus(browser, BrowserSearchFocus::None) ||
        update.changed;
    update.effect = BrowserSearchEffect::Reload;
    return update;
  }
  if (const std::optional<std::filesystem::path> target =
          resolvePathSearchTarget(browser)) {
    update.effect = BrowserSearchEffect::Navigate;
    update.navigationTarget = *target;
  }
  return update;
}

BrowserSearchUpdate blurBrowserSearch(BrowserState& browser) {
  BrowserSearchUpdate update;
  if (!browserSearchFocused(browser)) return update;
  update.handled = true;
  update.effect = browserFilterFocused(browser)
                      ? BrowserSearchEffect::Reload
                      : BrowserSearchEffect::None;
  update.changed =
      setBrowserSearchFocus(browser, BrowserSearchFocus::None);
  return update;
}

bool completeBrowserPathNavigation(BrowserState& browser) {
  if (!browserPathSearchFocused(browser)) return false;
  return setBrowserSearchFocus(browser, BrowserSearchFocus::None);
}
