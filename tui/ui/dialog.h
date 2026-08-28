#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "input_event.h"
#include "tui/ui/button_row.h"
#include "ui/text_grid/dialog_layout.h"

namespace tui_dialog {

using ButtonId = text_grid_dialog_layout::ButtonId;
using Button = text_grid_dialog_layout::Button;
using TextTone = text_grid_dialog_layout::TextTone;
using TextBlock = text_grid_dialog_layout::TextBlock;
using Content = text_grid_dialog_layout::Content;
using Bounds = text_grid_dialog_layout::Bounds;
using RenderLine = text_grid_dialog_layout::RenderLine;
using ButtonBounds = text_grid_dialog_layout::ButtonBounds;
using Layout = text_grid_dialog_layout::Layout;
using text_grid_dialog_layout::layoutContent;

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
