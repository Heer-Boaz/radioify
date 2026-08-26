#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "input_event.h"

namespace tui_popup_menu {

struct Anchor {
  int x = -1;
  int y = -1;
};

struct Bounds {
  int width = 0;
  int height = 0;
  int topInset = 0;
};

struct Layout {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int listY = 0;
  int visibleRows = 0;
  int firstItem = 0;
  bool valid = false;
};

struct Interaction {
  bool consumed = false;
  bool changed = false;
  bool dismissed = false;
  std::optional<std::size_t> activatedItem;
};

class Model {
 public:
  void open(std::vector<std::string> labels, Anchor anchor = {});
  bool dismiss();

  bool active() const { return active_; }
  int selected() const { return selected_; }
  const std::vector<std::string>& labels() const { return labels_; }

  Layout layout(const Bounds& bounds);
  Interaction handle(const InputEvent& event, const Bounds& bounds);

 private:
  void moveSelection(int direction, const Bounds& bounds);
  void ensureSelectionVisible(int visibleRows);
  Interaction activateSelected();

  std::vector<std::string> labels_;
  Anchor anchor_;
  int selected_ = 0;
  int firstVisible_ = 0;
  bool active_ = false;
};

}  // namespace tui_popup_menu
