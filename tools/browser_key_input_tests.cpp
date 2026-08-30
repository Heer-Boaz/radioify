#include "tui/ui/browser_keymap.h"
#include "tui/ui/browser_keyboard_input.h"
#include "tui/ui/browser_search.h"
#include "tui/ui/single_line_text_input.h"

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) {
    return true;
  }
  std::cerr << "browser_key_input_tests: " << message << '\n';
  return false;
}

KeyEvent key(WORD vk, char character = 0, DWORD control = 0,
             KeyPressKind pressKind = KeyPressKind::Initial,
             std::uint32_t repeatCount = 1) {
  return KeyEvent{vk, character, control, pressKind, repeatCount};
}

InputEvent keyInput(WORD vk, char character = 0, DWORD control = 0,
                    KeyPressKind pressKind = KeyPressKind::Initial,
                    std::uint32_t repeatCount = 1) {
  InputEvent event{};
  event.type = InputEvent::Type::Key;
  event.key = key(vk, character, control, pressKind, repeatCount);
  return event;
}

std::optional<browser_input::KeyAction> resolve(
    const KeyEvent& event, browser_input::ShortcutContext context) {
  return browser_input::resolveKeyAction(
      event, browser_input::shortcutContext(context));
}

}  // namespace

int main() {
  using browser_input::KeyAction;
  using browser_input::ShortcutContext;
  bool ok = true;

  ok &= expect(resolve(key('G', 'g', LEFT_CTRL_PRESSED),
                       ShortcutContext::SearchActivation) ==
                   KeyAction::BeginPathSearch,
               "Ctrl+G must enter path search through the browser keymap");
  ok &= expect(resolve(key('F', 'f', RIGHT_CTRL_PRESSED),
                       ShortcutContext::SearchActivation) ==
                   KeyAction::BeginFilter,
               "Ctrl+F must enter filter search through the browser keymap");
  ok &= expect(resolve(key(VK_DIVIDE, '/'),
                       ShortcutContext::SearchActivation) ==
                   KeyAction::BeginFilter &&
                   resolve(key('7', '/', SHIFT_PRESSED),
                           ShortcutContext::SearchActivation) ==
                       KeyAction::BeginFilter &&
                   !resolve(key(VK_DIVIDE, '/', LEFT_CTRL_PRESSED),
                            ShortcutContext::SearchActivation),
               "slash must open search without stealing control chords");
  ok &= expect(resolve(key('M', 'm'), ShortcutContext::Application) ==
                   KeyAction::TogglePitchMonitor &&
                   !resolve(key('M', 'm', LEFT_CTRL_PRESSED),
                            ShortcutContext::Application),
               "application bindings must not steal modified text keys");
  ok &= expect(resolve(key('S', 's', LEFT_ALT_PRESSED),
                       ShortcutContext::Navigation) ==
                   KeyAction::ToggleSortDirection &&
                   resolve(key('S', 's'), ShortcutContext::Navigation) ==
                       KeyAction::CycleSort,
               "sort mode and direction must be distinct typed shortcuts");
  ok &= expect(resolve(key(VK_RETURN, 0, LEFT_CTRL_PRESSED),
                       ShortcutContext::Navigation) ==
                   KeyAction::OpenSelectionMenu &&
                   resolve(key(VK_RETURN), ShortcutContext::Navigation) ==
                       KeyAction::ActivateSelection,
               "Enter variants must map to distinct selection intents");
  ok &= expect(resolve(key(VK_NEXT), ShortcutContext::Navigation) ==
                   KeyAction::PageDown,
               "the keymap must contain every browser navigation direction");
  ok &= expect(!resolve(key('S', 's', 0, KeyPressKind::AutoRepeat),
                        ShortcutContext::Navigation) &&
                   resolve(key(VK_DOWN, 0, 0, KeyPressKind::AutoRepeat),
                           ShortcutContext::Navigation) ==
                       KeyAction::MoveDown,
               "browser toggles must be edge-triggered while navigation "
               "remains repeatable");

  BrowserState routedSearch;
  beginBrowserFilter(routedSearch);
  const browser_keyboard_input::Capabilities routedCapabilities{true, true};
  browser_keyboard_input::Result routed = browser_keyboard_input::handle(
      routedSearch, keyInput('X', 'x'), routedCapabilities);
  const auto* routedText =
      std::get_if<browser_keyboard_input::SearchUpdate>(&routed);
  ok &= expect(routedText && routedText->update.changed &&
                   routedSearch.filter == "x",
               "focused search must own printable input through the keyboard "
               "mode workflow");

  routed = browser_keyboard_input::handle(
      routedSearch, keyInput(VK_SPACE, ' '), routedCapabilities);
  routedText = std::get_if<browser_keyboard_input::SearchUpdate>(&routed);
  ok &= expect(routedText && routedText->update.handled &&
                   routedText->update.changed &&
                   routedSearch.filter == "x ",
               "focused search must receive Space instead of toggling "
               "playback");

  routed = browser_keyboard_input::handle(
      routedSearch, keyInput(VK_LEFT, 0, LEFT_CTRL_PRESSED),
      routedCapabilities);
  routedText = std::get_if<browser_keyboard_input::SearchUpdate>(&routed);
  ok &= expect(routedText && routedText->update.handled &&
                   !routedText->update.changed &&
                   routedSearch.filter == "x ",
               "focused search must consume Ctrl+Left without publishing a "
               "playlist transport command");

  routed = browser_keyboard_input::handle(
      routedSearch, keyInput('G', 'g', LEFT_CTRL_PRESSED),
      routedCapabilities);
  const auto* searchActivation =
      std::get_if<browser_keyboard_input::BrowserAction>(&routed);
  ok &= expect(searchActivation &&
                   searchActivation->action == KeyAction::BeginPathSearch &&
                   routedSearch.filter == "x ",
               "browser search activation must route as a typed mode change "
               "before focused text consumes the chord");

  BrowserState routedPlayback;
  routed = browser_keyboard_input::handle(
      routedPlayback, keyInput(VK_SPACE, ' '), routedCapabilities);
  const auto* routedSpace =
      std::get_if<browser_keyboard_input::PlaybackCommand>(&routed);
  const auto* routedSpaceAction =
      routedSpace ? std::get_if<PlaybackAction>(&routedSpace->command)
                  : nullptr;
  ok &= expect(routedSpaceAction &&
                   *routedSpaceAction == PlaybackAction::TogglePause,
               "the same Space key must become playback only when no text "
               "field owns it");

  std::string text = "ab";
  auto edit = single_line_text_input::edit(text, key('C', 'c'));
  ok &= expect(edit.handled && edit.changed && text == "abc",
               "plain printable input must append text");
  edit = single_line_text_input::edit(
      text, key('Q', 'q', LEFT_CTRL_PRESSED));
  ok &= expect(!edit.handled && !edit.changed && text == "abc",
               "modified shortcuts must not leak into text fields");
  edit = single_line_text_input::edit(
      text, key('2', '@', LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED));
  ok &= expect(edit.handled && edit.changed && text == "abc@",
               "AltGr text input must remain available on international "
               "keyboard layouts");
  edit = single_line_text_input::edit(text, key(VK_BACK));
  ok &= expect(edit.handled && edit.changed && text == "abc",
               "Backspace must edit the shared single-line model");
  edit = single_line_text_input::edit(
      text, key('X', 'x', 0, KeyPressKind::AutoRepeat, 4));
  ok &= expect(edit.handled && edit.changed && text == "abcxxxx",
               "a batched printable repeat must produce every represented "
               "character without queue expansion");
  edit = single_line_text_input::edit(
      text, key(VK_BACK, 0, 0, KeyPressKind::AutoRepeat, 3));
  ok &= expect(edit.handled && edit.changed && text == "abcx",
               "a batched Backspace repeat must remove its represented "
               "characters in one edit operation");
  edit = single_line_text_input::edit(
      text, key(VK_RETURN, 0, 0, KeyPressKind::AutoRepeat, 5));
  ok &= expect(edit.handled &&
                   edit.intent == single_line_text_input::Intent::None,
               "repeated Enter must be consumed without committing a text "
               "workflow again");
  edit = single_line_text_input::edit(text, key(VK_RETURN));
  ok &= expect(edit.handled &&
                   edit.intent == single_line_text_input::Intent::Commit,
               "Enter must publish a commit intent");
  edit = single_line_text_input::edit(
      text, key(VK_ESCAPE, 0, 0, KeyPressKind::AutoRepeat, 2));
  ok &= expect(edit.handled &&
                   edit.intent == single_line_text_input::Intent::None,
               "repeated Escape must be consumed without cancelling a new "
               "workflow layer");
  edit = single_line_text_input::edit(text, key(VK_ESCAPE));
  ok &= expect(edit.handled &&
                   edit.intent == single_line_text_input::Intent::Cancel,
               "Escape must publish a cancel intent");

  BrowserState browser;
  browser.filter = "persistent";
  browser.pathSearch = "C:/temporary";
  ok &= expect(setBrowserSearchFocus(browser, BrowserSearchFocus::PathSearch) &&
                   browserPathSearchFocused(browser) &&
                   !browserFilterFocused(browser),
               "browser search must have one explicit focus owner");
  ok &= expect(setBrowserSearchFocus(browser, BrowserSearchFocus::Filter) &&
                   browserFilterFocused(browser) && browser.pathSearch.empty() &&
                   browser.filter == "persistent",
               "leaving path search must clear only its transient query");
  ok &= expect(!setBrowserSearchFocus(browser, BrowserSearchFocus::Filter),
               "setting the current focus must be idempotent");

  BrowserState filterSession;
  filterSession.filter = "original";
  ok &= expect(beginBrowserFilter(filterSession) &&
                   filterSession.filterBackup == "original",
               "entering filter search must capture one restore point");
  auto searchUpdate =
      handleBrowserSearchKey(filterSession, key('X', 'x'));
  ok &= expect(searchUpdate.handled && searchUpdate.changed &&
                   filterSession.filter == "originalx" &&
                   !beginBrowserFilter(filterSession) &&
                   filterSession.filterBackup == "original",
               "refocusing an active filter must not overwrite Escape undo");
  searchUpdate = handleBrowserSearchKey(filterSession, key(VK_ESCAPE));
  ok &= expect(searchUpdate.effect == BrowserSearchEffect::Reload &&
                   searchUpdate.changed &&
                   !browserSearchFocused(filterSession) &&
                   filterSession.filter == "original",
               "Escape must restore and reload the complete filter session");

  beginBrowserFilter(filterSession);
  handleBrowserSearchKey(filterSession, key('Y', 'y'));
  searchUpdate = blurBrowserSearch(filterSession);
  ok &= expect(searchUpdate.effect == BrowserSearchEffect::Reload &&
                   filterSession.filter == "originaly" &&
                   !browserSearchFocused(filterSession),
               "clicking away must commit the edited filter through Reload");

  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("radioify-browser-search-tests-" + std::to_string(stamp));
  const std::filesystem::path child = directory / "child";
  std::filesystem::create_directories(child);
  BrowserState pathSession;
  beginBrowserPathSearch(pathSession);
  pathSession.pathSearch = child.string();
  searchUpdate = handleBrowserSearchKey(pathSession, key(VK_RETURN));
  ok &= expect(searchUpdate.handled &&
                   searchUpdate.effect == BrowserSearchEffect::Navigate &&
                   searchUpdate.navigationTarget == child &&
                   browserPathSearchFocused(pathSession) &&
                   completeBrowserPathNavigation(pathSession) &&
                   !browserSearchFocused(pathSession) &&
                   pathSession.pathSearch.empty(),
               "path search must publish typed navigation before clearing "
               "its query on accepted navigation");
  std::error_code cleanupError;
  std::filesystem::remove_all(directory, cleanupError);

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
