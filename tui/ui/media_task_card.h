#pragma once

#include "tui/consolescreen.h"
#include "tui/ui/media_task_panel.h"

struct MediaTaskCardStyles {
  Style background;
  Style title;
  Style secondary;
  Style progress;
  Style button;
  Style selectedButton;
};

void drawMediaTaskCard(ConsoleScreen& screen, int screenWidth,
                       int screenHeight, int top,
                       const MediaTaskCardModel& model,
                       const tui_media_task_panel::State& state,
                       const MediaTaskCardStyles& styles);
