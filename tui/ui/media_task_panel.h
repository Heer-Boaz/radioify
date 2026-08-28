#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "tui/input_event.h"
#include "tui/ui/button_row.h"
#include "tui/ui/media_task_presentation.h"

namespace tui_media_task_panel {

enum class Action : tui_button_row::ButtonId {
  Cancel = 1,
};

inline constexpr tui_dialog::ButtonId kCancelTaskButton = 2;
inline constexpr tui_dialog::ButtonId kKeepRunningButton = 3;

struct Bounds {
  int width = 0;
  int height = 0;
  int top = 0;
};

struct Layout {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int innerWidth = 0;
  int progressY = 0;
  tui_button_row::Layout buttons;
  bool valid = false;

  bool contains(int pointerX, int pointerY) const;
};

struct Interaction {
  bool consumed = false;
  bool changed = false;
  std::optional<Action> activatedAction;
};

class State {
 public:
  void synchronize(bool taskActive);
  Interaction handle(const InputEvent& event, const Bounds& bounds,
                     const MediaTaskCardModel& task);

  std::optional<std::size_t> hoveredButton() const {
    return hoveredButton_;
  }

 private:
  std::optional<std::size_t> hoveredButton_;
};

std::vector<tui_button_row::Button> actionsFor(
    const MediaTaskCardModel& task);
Layout layout(const Bounds& bounds, const MediaTaskCardModel& task);
tui_dialog::Content cancellationDialog(const MediaTaskCardModel& task);

}  // namespace tui_media_task_panel
