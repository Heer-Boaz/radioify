#include "tui/ui/browser_keymap.h"
#include "tui/ui/browser_search.h"
#include "tui/ui/single_line_text_input.h"

#include <cstdlib>
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

KeyEvent key(WORD vk, char character = 0, DWORD control = 0) {
  return KeyEvent{vk, character, control};
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
  edit = single_line_text_input::edit(text, key(VK_RETURN));
  ok &= expect(edit.handled &&
                   edit.intent == single_line_text_input::Intent::Commit,
               "Enter must publish a commit intent");
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

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
