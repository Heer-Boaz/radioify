#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "input_event.h"

namespace tui_command_palette {

class Command {
 public:
  Command(std::string label, std::string hotkey);

  const std::string& label() const { return label_; }
  const std::string& hotkey() const { return hotkey_; }

 private:
  std::string label_;
  std::string hotkey_;
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
  int innerWidth = 0;
  int inputY = 0;
  int listY = 0;
  int visibleRows = 0;
  int firstFilteredItem = 0;
  bool valid = false;
};

struct Interaction {
  bool consumed = false;
  bool changed = false;
  bool dismissed = false;
  std::optional<std::size_t> activatedCommand;
};

class Model {
 public:
  void open();
  bool dismiss();

  bool active() const { return active_; }
  const std::string& query() const { return query_; }
  int selectedFilteredItem() const { return selected_; }
  const std::vector<std::size_t>& filteredCommandIndices() const {
    return filteredCommandIndices_;
  }

  Layout layout(const std::vector<Command>& commands, const Bounds& bounds);
  Interaction handle(const InputEvent& event,
                     const std::vector<Command>& commands,
                     const Bounds& bounds);

 private:
  void refreshFilter(const std::vector<Command>& commands);
  void resetSelection();
  void moveSelection(int offset, int visibleRows);
  void ensureSelectionVisible(int visibleRows);
  Interaction activateSelected();

  std::string query_;
  std::vector<std::size_t> filteredCommandIndices_;
  int selected_ = 0;
  int firstVisible_ = 0;
  bool active_ = false;
};

}  // namespace tui_command_palette
