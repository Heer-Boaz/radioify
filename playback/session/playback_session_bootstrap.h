#pragma once

#include <filesystem>

#include "consoleinput.h"
#include "consolescreen.h"
#include "player.h"

enum class PlaybackSessionBootstrapOutcome {
  ContinueVideo,
  PlayAudioOnly,
  Handled,
};

PlaybackSessionBootstrapOutcome bootstrapPlaybackSession(
    const std::filesystem::path& file, ConsoleInput& input,
    ConsoleScreen& screen, const Style& baseStyle, const Style& accentStyle,
    const Style& dimStyle, const Style& progressEmptyStyle,
    const Style& progressFrameStyle, const Color& progressStart,
    const Color& progressEnd, bool enableAudio, bool enableAscii,
    Player& player, bool* quitAppRequested = nullptr);
