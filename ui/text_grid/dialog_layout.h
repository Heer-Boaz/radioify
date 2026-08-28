#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ui/text_grid/button_layout.h"

namespace text_grid_dialog_layout {

using ButtonId = text_grid_button_layout::ButtonId;
using Button = text_grid_button_layout::Button;
using ButtonBounds = text_grid_button_layout::Placement;

enum class TextTone : std::uint8_t {
  Normal,
  Emphasis,
  Secondary,
  Error,
};

struct TextBlock {
  std::string text;
  TextTone tone = TextTone::Normal;
};

struct Content {
  std::string title;
  std::vector<TextBlock> text;
  std::vector<Button> buttons;
  std::optional<ButtonId> initiallySelectedButton;
};

struct Bounds {
  int width = 0;
  int height = 0;
  int topInset = 0;
};

struct RenderLine {
  std::string text;
  TextTone tone = TextTone::Normal;
};

struct Layout {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int innerWidth = 0;
  int titleY = 0;
  int contentY = 0;
  int buttonY = 0;
  int visibleContentRows = 0;
  int firstContentLine = 0;
  std::vector<RenderLine> contentLines;
  std::vector<ButtonBounds> buttons;
  bool valid = false;
};

// Pure responsive geometry for a modal text-grid decision. Browser, ASCII and
// native playback each retain their own lifecycle, input and rendering adapter.
Layout layoutContent(const Content& content, std::size_t selectedButton,
                     int firstVisibleLine, const Bounds& bounds);

}  // namespace text_grid_dialog_layout
