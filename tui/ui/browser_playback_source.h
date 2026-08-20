#pragma once

#include <vector>

#include "app/playback_controller.h"
#include "browser_model.h"

namespace browser_playback_source {

playback_controller::Source capture(const std::vector<BrowserEntry>& entries);

}  // namespace browser_playback_source
