#pragma once

#include <variant>

#include "playback/session/problem.h"

namespace playback_session {

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
