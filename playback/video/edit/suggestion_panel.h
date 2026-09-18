#pragma once

#include <optional>
#include <string>
#include <vector>

#include "playback/video/edit/command.h"
#include "playback/video/edit/view.h"

namespace playback_video_edit {

inline constexpr const char* kSuggestionPanelTitle = "Edit suggestions";

struct SuggestionAction {
  Command command;
  std::string label;
  bool enabled = true;
  bool active = false;
};

// Owns task presentation policy. Menus and toolbars render these actions;
// neither surface interprets analysis lifecycle or review eligibility.
struct SuggestionPresentation {
  struct Paragraph {
    std::string text;
    bool accent = false;
  };
  std::vector<SuggestionAction> actions;
  std::vector<Paragraph> introduction;
};

SuggestionPresentation buildSuggestionPresentation(const EditSnapshot& edit);

struct SuggestionPanelLayout {
  struct Line {
    std::string text;
    bool accent = false;
    std::optional<uint64_t> suggestionId;
    bool selected = false;
  };
  int x = 0, y = 0, width = 0, height = 0;
  int scrollOffset = 0, maximumScrollOffset = 0;
  int closeColumn = 0;
  std::vector<Line> lines;
  bool drawable() const { return width >= 20 && height >= 4; }
};

// One responsive content layout for the console, video window and PiP.
// Task controls remain in the editor's command bar below the video content.
SuggestionPanelLayout layoutSuggestionPanel(const EditSnapshot& edit,
                                             int columns, int rows,
                                             int chromeTopY);

}  // namespace playback_video_edit
