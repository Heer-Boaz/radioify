#pragma once

#include <filesystem>
#include <functional>

#include "core/runtime_defaults.h"
#include "consoleinput.h"
#include "consolescreen.h"
#include "playback/system_media_transport_controls.h"
#include "playback/playback_transport.h"
#include "playback/session/playback_session_state.h"

struct VideoPlaybackConfig {
  bool enableAscii = kDefaultAsciiPlaybackEnabled;
  bool enableAudio = kDefaultAudioPlaybackEnabled;
  bool debugOverlay = false;
  bool enableWindow = kDefaultWindowPlaybackEnabled;
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
                    bool* quitAppRequested = nullptr,
                    PlaybackSystemControls* systemControls = nullptr,
                    std::function<bool(PlaybackTransportCommand)>
                        requestTransportCommand = {},
                    PlaybackSessionContinuationState* continuityState = nullptr);
