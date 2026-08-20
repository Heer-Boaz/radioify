#pragma once

#include <filesystem>

#include "core/runtime_defaults.h"

struct VideoPlaybackConfig {
  bool enableAscii = kDefaultAsciiPlaybackEnabled;
  bool enableAudio = kDefaultAudioPlaybackEnabled;
  bool debugOverlay = false;
  bool enableWindow = kDefaultWindowPlaybackEnabled;
};

void configureFfmpegVideoLog(const std::filesystem::path& path);
