#include "dialog_renderer.h"

#include <algorithm>
#include <string>

#include "tui/ui/ui_helpers.h"

namespace tui_dialog {
namespace {

Style lineStyle(TextTone tone, const Styles& styles) {
  switch (tone) {
    case TextTone::Normal:
      return styles.normal;
    case TextTone::Emphasis:
      return styles.emphasis;
    case TextTone::Secondary:
      return styles.secondary;
    case TextTone::Error:
      return styles.error;
  }
  return styles.normal;
}

}  // namespace

void draw(ConsoleScreen& screen, Model& model, const Bounds& bounds,
          const Styles& styles) {
  const Layout layout = model.layout(bounds);
  if (!layout.valid) {
    return;
  }

  for (int row = 0; row < layout.height; ++row) {
    screen.writeRun(layout.x, layout.y + row, layout.width, L' ',
                    styles.background);
  }
  screen.writeChar(layout.x, layout.y, L'+', styles.border);
  screen.writeRun(layout.x + 1, layout.y, layout.width - 2, L'-',
                  styles.border);
  screen.writeChar(layout.x + layout.width - 1, layout.y, L'+',
                   styles.border);
  screen.writeChar(layout.x, layout.y + layout.height - 1, L'+',
                   styles.border);
  screen.writeRun(layout.x + 1, layout.y + layout.height - 1,
                  layout.width - 2, L'-', styles.border);
  screen.writeChar(layout.x + layout.width - 1,
                   layout.y + layout.height - 1, L'+', styles.border);
  for (int row = 1; row < layout.height - 1; ++row) {
    screen.writeChar(layout.x, layout.y + row, L'|', styles.border);
    screen.writeChar(layout.x + layout.width - 1, layout.y + row, L'|',
                     styles.border);
  }

  screen.writeText(layout.x + 2, layout.titleY,
                   fitLine(model.content().title, layout.innerWidth),
                   styles.title);

  const int endLine = std::min(
      static_cast<int>(layout.contentLines.size()),
      layout.firstContentLine + layout.visibleContentRows);
  for (int line = layout.firstContentLine; line < endLine; ++line) {
    const RenderLine& contentLine =
        layout.contentLines[static_cast<std::size_t>(line)];
    screen.writeText(
        layout.x + 2,
        layout.contentY + line - layout.firstContentLine,
        fitLine(contentLine.text, layout.innerWidth),
        lineStyle(contentLine.tone, styles));
  }
  if (layout.firstContentLine > 0) {
    screen.writeChar(layout.x + layout.width - 2, layout.contentY, L'^',
                     styles.border);
  }
  if (endLine < static_cast<int>(layout.contentLines.size())) {
    screen.writeChar(layout.x + layout.width - 2,
                     layout.contentY + layout.visibleContentRows - 1,
                     L'v', styles.border);
  }

  for (const ButtonBounds& buttonBounds : layout.buttons) {
    if (buttonBounds.index >= model.content().buttons.size()) {
      continue;
    }
    const Button& button = model.content().buttons[buttonBounds.index];
    std::string label = "[ " + button.label + " ]";
    label = fitLine(label, buttonBounds.width);
    screen.writeRun(
        buttonBounds.x, layout.buttonY, buttonBounds.width, L' ',
        buttonBounds.index == model.selectedButton()
            ? styles.selectedButton
            : styles.button);
    screen.writeText(
        buttonBounds.x, layout.buttonY, label,
        buttonBounds.index == model.selectedButton()
            ? styles.selectedButton
            : styles.button);
  }
}

}  // namespace tui_dialog
