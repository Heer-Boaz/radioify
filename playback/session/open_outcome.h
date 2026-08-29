#pragma once

#include <string>
#include <variant>

namespace playback_session {

struct Problem {
  std::string message;
  std::string detail;
};

struct OpenReady {};
struct OpenCancelled {};
struct OpenQuitApplication {};

struct OpenAudioFallback {
  Problem reason;
};

struct OpenFailure {
  Problem problem;
};

using OpenOutcome = std::variant<OpenReady, OpenCancelled,
                                 OpenQuitApplication, OpenAudioFallback,
                                 OpenFailure>;

} // namespace playback_session
