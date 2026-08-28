#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "input_event.h"
#include "tui/ui/button_row.h"

namespace tui_dialog {

using ButtonId = tui_button_row::ButtonId;

struct DialogId {
  std::uint64_t value = 0;

  constexpr explicit operator bool() const { return value != 0; }
  friend constexpr bool operator==(DialogId left, DialogId right) {
    return left.value == right.value;
  }
  friend constexpr bool operator!=(DialogId left, DialogId right) {
    return !(left == right);
  }
};

struct ButtonActivation {
  DialogId dialog;
  ButtonId button = 0;
};

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

using Button = tui_button_row::Button;

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

using ButtonBounds = tui_button_row::Placement;

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

struct Interaction {
  bool consumed = false;
  bool changed = false;
  std::optional<DialogId> dismissedDialog;
  std::optional<ButtonActivation> activation;
};

// Input-modal dialog model for the browser shell. It never owns a nested
// event loop: background work, window messages and redraws keep flowing while
// this model exclusively consumes browser input.
class Model {
 public:
  DialogId open(Content content);
  bool dismiss();
  bool dismiss(DialogId expectedDialog);

  bool active() const { return active_; }
  std::optional<DialogId> activeId() const {
    return active_ ? std::optional<DialogId>(activeDialog_) : std::nullopt;
  }
  const Content& content() const { return content_; }
  std::size_t selectedButton() const { return selectedButton_; }

  Layout layout(const Bounds& bounds);
  Interaction handle(const InputEvent& event, const Bounds& bounds);

 private:
  void selectAdjacentButton(int direction);
  void scrollBy(int rows, const Layout& layout);
  Interaction activateSelected();

  Content content_;
  std::size_t selectedButton_ = 0;
  int firstVisibleLine_ = 0;
  bool active_ = false;
  tui_button_row::PointerState buttonPointer_;
  DialogId activeDialog_;
  std::uint64_t nextDialogId_ = 1;
};

}  // namespace tui_dialog
