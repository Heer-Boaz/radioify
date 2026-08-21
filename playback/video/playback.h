#pragma once

#include <filesystem>

#include "core/runtime_defaults.h"

struct VideoPlaybackConfig {
  bool enableAscii = kDefaultAsciiPlaybackEnabled;
  bool enableAudio = kDefaultAudioPlaybackEnabled;
  bool debugOverlay = false;
};

void configureFfmpegVideoLog(const std::filesystem::path& path);
