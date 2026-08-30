#pragma once

#include <string>

namespace playback_session {

struct Problem {
  std::string message;
  std::string detail;
};

}  // namespace playback_session
