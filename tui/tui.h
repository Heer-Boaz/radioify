#ifndef TUI_H
#define TUI_H

#include "app_common.h"

namespace playback_queue {
class Queue;
}

int runTui(Options o, playback_queue::Queue& playbackQueue);

#endif  // TUI_H
