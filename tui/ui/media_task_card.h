#pragma once

#include "tui/consolescreen.h"
#include "tui/ui/media_task_presentation.h"

struct MediaTaskCardStyles {
  Style background;
  Style title;
  Style secondary;
  Style progress;
};

void drawMediaTaskCard(ConsoleScreen& screen, int screenWidth,
                       int screenHeight, int top,
                       const MediaTaskCardModel& model,
                       const MediaTaskCardStyles& styles);
