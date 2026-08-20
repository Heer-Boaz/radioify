#ifndef TUI_H
#define TUI_H

#include "app_common.h"

namespace playback_controller {
class Controller;
}

int runTui(Options o, playback_controller::Controller& playbackController);

#endif  // TUI_H
