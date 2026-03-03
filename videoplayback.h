#pragma once

#include <filesystem>

#include "consoleinput.h"
#include "consolescreen.h"

struct VideoPlaybackConfig {
  bool enableAscii = true;
  bool enableAudio = true;
  bool debugOverlay = false;
  bool enableWindow = false;
};

void configureFfmpegVideoLog(const std::filesystem::path& path);

bool showAsciiVideo(const std::filesystem::path& file,
                    ConsoleInput& input,
                    ConsoleScreen& screen,
                    const Style& baseStyle,
                    const Style& accentStyle,
                    const Style& dimStyle,
                    const Style& progressEmptyStyle,
                    const Style& progressFrameStyle,
                    const Color& progressStart,
                    const Color& progressEnd,
                    const VideoPlaybackConfig& config,
                    bool* quitAppRequested = nullptr);
