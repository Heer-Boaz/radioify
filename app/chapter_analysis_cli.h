#pragma once

#include <filesystem>

// Runs the normal chapter-analysis service without constructing a playback
// session or presentation surface. The final machine-readable document is
// written to stdout; progress and diagnostics are written to stderr.
int runChapterAnalysisCli(const std::filesystem::path& file);
