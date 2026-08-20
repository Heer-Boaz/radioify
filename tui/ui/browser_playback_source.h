#pragma once

#include <vector>

#include "browser_model.h"
#include "playback_queue.h"

namespace browser_playback_source {

playback_queue::Source capture(const std::vector<BrowserEntry>& entries);

}  // namespace browser_playback_source
