#pragma once

#include <functional>
#include <string_view>

#include "core/diagnostic_log.h"

namespace audio_separation {

// Backends publish structured diagnostics to their job owner. They never
// choose a file, terminal, or UI destination themselves.
using DiagnosticReporter = std::function<void(
    DiagnosticLevel, std::string_view, std::string_view)>;

inline void reportDiagnostic(const DiagnosticReporter& reporter,
                             DiagnosticLevel level,
                             std::string_view component,
                             std::string_view message) {
  if (reporter) reporter(level, component, message);
}

}  // namespace audio_separation
