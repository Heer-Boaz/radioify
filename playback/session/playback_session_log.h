#pragma once

#include <filesystem>
#include <fstream>
#include <string>

class ConsoleScreen;

struct PerfLog {
  std::ofstream file;
  std::string buffer;
  bool enabled = false;
};

bool perfLogOpen(PerfLog* log, const std::filesystem::path& path,
                 std::string* error);

void perfLogFlush(PerfLog* log);

void perfLogAppendf(PerfLog* log, const char* fmt, ...);

void perfLogClose(PerfLog* log);

void finalizeVideoPlayback(ConsoleScreen& screen, bool fullRedrawEnabled,
                           PerfLog* log);
