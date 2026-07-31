#pragma once

#include <memory>
#include <string>

namespace playback_overlay {

struct PlaybackOsdSnapshot {
  bool controlsVisible = false;
  std::shared_ptr<const std::string> message;
};

}  // namespace playback_overlay
