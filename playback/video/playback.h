#pragma once

#include <filesystem>

#include "core/runtime_defaults.h"

enum class SystemMediaCommandOwner {
  NativeWindowFallback,
  SystemMediaTransportControls,
};

struct VideoPlaybackConfig {
  bool enableAscii = kDefaultAsciiPlaybackEnabled;
  bool enableAudio = kDefaultAudioPlaybackEnabled;
  bool debugOverlay = false;
  SystemMediaCommandOwner systemMediaCommandOwner =
      SystemMediaCommandOwner::NativeWindowFallback;
};

void configureFfmpegVideoLog(const std::filesystem::path& path);
