#ifndef PLAYBACK_DIALOG_H
#define PLAYBACK_DIALOG_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

#include "consoleinput.h"
#include "consolescreen.h"
#include "ui_helpers.h"

namespace playback_dialog {

enum class DialogResult {
  Confirmed,
  Cancelled,
};

namespace {

inline void renderDialogBody(ConsoleScreen& screen,
                            const Style& baseStyle, const Style& accentStyle,
                            const Style& dimStyle, const std::string& title,
                            const std::string& message,
                            const std::string& detail,
                            const std::string& footer) {
  std::vector<std::string> lines;
  if (!title.empty()) {
    lines.push_back(title);
  }
  if (!message.empty()) {
    lines.push_back(message);
  }
  if (!detail.empty()) {
    lines.push_back(detail);
  }

  screen.updateSize();
  int width = std::max(20, screen.width());
  int height = std::max(10, screen.height());

  int contentWidth = 0;
  for (const auto& line : lines) {
    contentWidth = std::max(contentWidth, utf8CodepointCount(line));
  }
  int footerWidth = footer.empty() ? 0 : utf8CodepointCount(footer);
  int maxPopupWidth = std::max(7, width - 2);
  int popupWidth =
      std::clamp(std::max(contentWidth, footerWidth) + 4, 7, maxPopupWidth);
  popupWidth = std::max(7, popupWidth);
  int contentHeight = static_cast<int>(lines.size()) + 2;
  int popupHeight = std::clamp(contentHeight + 3, 7, height - 2);
  if (!footer.empty()) {
    popupHeight = std::clamp(popupHeight + 1, 7, height - 2);
  }

  int x0 = std::max(0, (width - popupWidth) / 2);
  int y0 = std::max(0, (height - popupHeight) / 2);
  int innerWidth = popupWidth - 2;

  for (int y = 0; y < popupHeight; ++y) {
    screen.writeRun(x0, y0 + y, popupWidth, L' ', baseStyle);
  }
  screen.writeChar(x0, y0, L'+', dimStyle);
  screen.writeRun(x0 + 1, y0, popupWidth - 2, L'-', dimStyle);
  screen.writeChar(x0 + popupWidth - 1, y0, L'+', dimStyle);
  screen.writeChar(x0, y0 + popupHeight - 1, L'+', dimStyle);
  screen.writeRun(x0 + 1, y0 + popupHeight - 1, popupWidth - 2, L'-',
                 dimStyle);
  screen.writeChar(x0 + popupWidth - 1, y0 + popupHeight - 1, L'+',
                  dimStyle);
  for (int y = 1; y < popupHeight - 1; ++y) {
    screen.writeChar(x0, y0 + y, L'|', dimStyle);
    screen.writeChar(x0 + popupWidth - 1, y0 + y, L'|', dimStyle);
  }

  int y = y0 + 1;
  for (size_t i = 0; i < lines.size(); ++i) {
    const auto& line = lines[static_cast<size_t>(i)];
    if (y >= y0 + popupHeight - 1) {
      break;
    }
    Style style = baseStyle;
    if (!title.empty() && i == 0) {
      style = accentStyle;
    } else if (!detail.empty() && i + 1 == lines.size()) {
      style = dimStyle;
    }
    screen.writeText(x0 + 1, y++, fitLine(line, innerWidth), style);
  }
  if (!footer.empty() && y0 + popupHeight - 2 >= y0 + 1) {
    screen.writeText(x0 + 1, y0 + popupHeight - 2, fitLine(footer, innerWidth),
                     dimStyle);
  }
  screen.draw();
}

}  // namespace

inline void showInfoDialog(ConsoleInput& input, ConsoleScreen& screen,
                          const Style& baseStyle, const Style& accentStyle,
                          const Style& dimStyle, const std::string& title,
                          const std::string& message,
                          const std::string& detail,
                          const std::string& footer) {
  renderDialogBody(screen, baseStyle, accentStyle, dimStyle, title, message,
                  detail, footer);

  InputEvent ev{};
  while (true) {
    while (input.poll(ev)) {
      if (ev.type == InputEvent::Type::Key) {
        const KeyEvent& key = ev.key;
        if (key.vk == VK_RETURN || key.vk == VK_SPACE || key.vk == VK_ESCAPE ||
            key.vk == VK_BROWSER_BACK || key.vk == VK_BACK) {
          return;
        }
      }
      if (ev.type == InputEvent::Type::Mouse) {
        if (ev.mouse.buttonState != 0) return;
      }
      if (ev.type == InputEvent::Type::Resize) {
        renderDialogBody(screen, baseStyle, accentStyle, dimStyle, title,
                        message, detail, footer);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

inline DialogResult showConfirmDialog(ConsoleInput& input, ConsoleScreen& screen,
                                    const Style& baseStyle,
                                    const Style& accentStyle,
                                    const Style& dimStyle,
                                    const std::string& title,
                                    const std::string& message,
                                    const std::string& detail,
                                    const std::string& footer) {
  renderDialogBody(screen, baseStyle, accentStyle, dimStyle, title, message,
                  detail, footer);

  InputEvent ev{};
  while (true) {
    while (input.poll(ev)) {
      if (ev.type == InputEvent::Type::Mouse) {
        if (ev.mouse.buttonState != 0) return DialogResult::Confirmed;
      }
      if (ev.type == InputEvent::Type::Key) {
        const KeyEvent& key = ev.key;
        if (key.vk == VK_RETURN || key.vk == VK_SPACE) {
          return DialogResult::Confirmed;
        }
        if (key.vk == VK_ESCAPE || key.vk == VK_BROWSER_BACK ||
            key.vk == VK_BACK) {
          return DialogResult::Cancelled;
        }
      }
      if (ev.type == InputEvent::Type::Resize) {
        renderDialogBody(screen, baseStyle, accentStyle, dimStyle, title,
                        message, detail, footer);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

}  // namespace playback_dialog

#endif  // PLAYBACK_DIALOG_H
