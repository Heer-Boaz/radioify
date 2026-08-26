#include "tui/ui/command_palette.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) {
    return true;
  }
  std::cerr << "command_palette_tests: " << message << '\n';
  return false;
}

InputEvent keyEvent(WORD key, char character = 0, DWORD control = 0) {
  InputEvent event;
  event.type = InputEvent::Type::Key;
  event.key.vk = key;
  event.key.ch = character;
  event.key.control = control;
  return event;
}

InputEvent mousePress(int x, int y) {
  InputEvent event;
  event.type = InputEvent::Type::Mouse;
  event.mouse.kind = MouseEventKind::Press;
  event.mouse.pos.X = static_cast<SHORT>(x);
  event.mouse.pos.Y = static_cast<SHORT>(y);
  event.mouse.buttons = MouseButtons::Left;
  return event;
}

std::vector<tui_command_palette::Command> commands(int* invoked) {
  std::vector<tui_command_palette::Command> result;
  result.emplace_back("Play/Pause", "Space", [invoked] { *invoked = 1; });
  result.emplace_back("Picture-in-Picture", "Ctrl+P",
                      [invoked] { *invoked = 2; });
  result.emplace_back("Quit", "Q", [invoked] { *invoked = 3; });
  return result;
}

}  // namespace

int main() {
  bool ok = true;
  int invoked = 0;
  const std::vector<tui_command_palette::Command> availableCommands =
      commands(&invoked);
  tui_command_palette::Bounds bounds;
  bounds.width = 24;
  bounds.height = 8;
  bounds.topInset = 1;

  tui_command_palette::Model palette;
  palette.open();
  tui_command_palette::Layout layout =
      palette.layout(availableCommands, bounds);
  ok &= expect(layout.valid && layout.width == 24 && layout.x == 0 &&
                   layout.visibleRows == 3,
               "palette geometry must fit a narrow viewport");

  palette.handle(keyEvent('P', 'p'), availableCommands, bounds);
  palette.handle(keyEvent('Y', 'y'), availableCommands, bounds);
  layout = palette.layout(availableCommands, bounds);
  ok &= expect(palette.query() == "py" &&
                   palette.filteredCommandIndices().size() == 1 &&
                   palette.filteredCommandIndices()[0] == 0,
               "fuzzy filtering must be case-insensitive and ordered");

  tui_command_palette::Interaction interaction = palette.handle(
      keyEvent(VK_RETURN), availableCommands, bounds);
  ok &= expect(interaction.activatedCommand == 0 && interaction.dismissed &&
                   !palette.active(),
               "Enter must return the selected command and close the palette");
  if (interaction.activatedCommand) {
    availableCommands[*interaction.activatedCommand].run();
  }
  ok &= expect(invoked == 1,
               "the returned command index must address the original catalog");

  palette.open();
  interaction = palette.handle(keyEvent('P', 'p', LEFT_CTRL_PRESSED),
                               availableCommands, bounds);
  ok &= expect(interaction.consumed && palette.query().empty(),
               "modified shortcut keys must not become search text");

  palette.handle(keyEvent(VK_DOWN), availableCommands, bounds);
  palette.handle(keyEvent(VK_DOWN), availableCommands, bounds);
  palette.handle(keyEvent(VK_DOWN), availableCommands, bounds);
  ok &= expect(palette.selectedFilteredItem() == 2,
               "keyboard selection must clamp at the final result");

  layout = palette.layout(availableCommands, bounds);
  interaction = palette.handle(mousePress(0, 0), availableCommands, bounds);
  ok &= expect(interaction.dismissed && !palette.active(),
               "a click outside the palette must dismiss the modal");

  palette.open();
  palette.handle(keyEvent('Z', 'z'), availableCommands, bounds);
  layout = palette.layout(availableCommands, bounds);
  interaction =
      palette.handle(keyEvent(VK_RETURN), availableCommands, bounds);
  ok &= expect(palette.active() && !interaction.activatedCommand,
               "Enter must not close the palette when there are no matches");

  interaction = palette.handle(inputActionEvent(InputAction::Back),
                               availableCommands, bounds);
  ok &= expect(interaction.consumed && interaction.dismissed &&
                   !palette.active(),
               "the shared Back action must dismiss the command palette");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
