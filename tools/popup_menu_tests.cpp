#include "tui/ui/popup_menu.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) {
    return true;
  }
  std::cerr << "popup_menu_tests: " << message << '\n';
  return false;
}

InputEvent keyEvent(WORD key) {
  InputEvent event;
  event.type = InputEvent::Type::Key;
  event.key.vk = key;
  return event;
}

InputEvent mouseEvent(MouseEventKind kind, int x, int y,
                      MouseButtons buttons = MouseButtons::None) {
  InputEvent event;
  event.type = InputEvent::Type::Mouse;
  event.mouse.kind = kind;
  event.mouse.pos.X = static_cast<SHORT>(x);
  event.mouse.pos.Y = static_cast<SHORT>(y);
  event.mouse.buttons = buttons;
  return event;
}

std::vector<std::string> labels() {
  return {"First", "Second", "Third", "Fourth", "Fifth", "Sixth"};
}

}  // namespace

int main() {
  bool ok = true;
  tui_popup_menu::Bounds bounds;
  bounds.width = 20;
  bounds.height = 6;
  bounds.topInset = 1;

  tui_popup_menu::Model menu;
  tui_popup_menu::Anchor anchoredOutsideViewport;
  anchoredOutsideViewport.x = 90;
  anchoredOutsideViewport.y = 90;
  menu.open(labels(), anchoredOutsideViewport);
  tui_popup_menu::Layout layout = menu.layout(bounds);
  ok &= expect(layout.valid && layout.x == 2 && layout.y == 1 &&
                   layout.width == 18 && layout.visibleRows == 3,
               "popup geometry must remain inside the available viewport");

  for (int index = 0; index < 4; ++index) {
    const tui_popup_menu::Interaction interaction =
        menu.handle(keyEvent(VK_DOWN), bounds);
    ok &= expect(interaction.consumed && interaction.changed,
                 "arrow navigation must be consumed and invalidate the view");
  }
  layout = menu.layout(bounds);
  ok &= expect(menu.selected() == 4 && layout.firstItem == 2,
               "keyboard navigation must keep the selected item visible");

  tui_popup_menu::Interaction interaction =
      menu.handle(keyEvent(VK_RETURN), bounds);
  ok &= expect(interaction.activatedItem == 4 && interaction.dismissed &&
                   !menu.active(),
               "Enter must activate the selected item and close the popup");

  menu.open(labels());
  menu.handle(keyEvent(VK_UP), bounds);
  ok &= expect(menu.selected() == 5,
               "keyboard navigation must wrap at the first item");

  layout = menu.layout(bounds);
  interaction = menu.handle(
      mouseEvent(MouseEventKind::Move, layout.x + 1, layout.listY), bounds);
  ok &= expect(interaction.consumed && menu.selected() == layout.firstItem,
               "pointer movement must select the visible row under it");
  interaction = menu.handle(
      mouseEvent(MouseEventKind::Press, layout.x + 1, layout.listY,
                 MouseButtons::Left),
      bounds);
  ok &= expect(interaction.activatedItem ==
                   static_cast<std::size_t>(layout.firstItem) &&
                   !menu.active(),
               "a left press on a row must activate that row");

  menu.open(labels());
  interaction = menu.handle(
      mouseEvent(MouseEventKind::Press, 0, 0, MouseButtons::Left), bounds);
  ok &= expect(interaction.dismissed && !menu.active(),
               "a left press outside the popup must dismiss it");

  menu.open(labels());
  InputEvent back = inputActionEvent(InputAction::Back);
  interaction = menu.handle(back, bounds);
  ok &= expect(interaction.consumed && interaction.dismissed && !menu.active(),
               "the shared Back action must dismiss an active popup");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
