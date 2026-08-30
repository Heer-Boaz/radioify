#pragma once

#include <filesystem>

#include "core/runtime_defaults.h"
#include "playback/input/media_keys.h"

struct VideoPlaybackConfig {
  bool enableAscii = kDefaultAsciiPlaybackEnabled;
  bool enableAudio = kDefaultAudioPlaybackEnabled;
  bool debugOverlay = false;
  SystemMediaCommandOwner systemMediaCommandOwner =
      SystemMediaCommandOwner::LocalInputFallback;
};

void configureFfmpegVideoLog(const std::filesystem::path& path);
