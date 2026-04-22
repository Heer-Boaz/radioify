#pragma once

#include <filesystem>
#include <vector>

#include "playback/playback_transport.h"
#include "playback_session_input.h"

namespace playback_session_handoff {

bool requestTransportHandoff(
    const playback_session_input::PlaybackInputView& view,
    playback_session_input::PlaybackInputSignals& signals,
    PlaybackTransportCommand command);
bool requestOpenFilesHandoff(
    const playback_session_input::PlaybackInputView& view,
    playback_session_input::PlaybackInputSignals& signals,
    const std::vector<std::filesystem::path>& files);

}  // namespace playback_session_handoff
