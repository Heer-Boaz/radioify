#pragma once

#include <filesystem>
#include <vector>

#include "input.h"

namespace playback_session_file_drop {

bool handleInputEvent(const playback_session_input::PlaybackInputView& view,
                      playback_session_input::PlaybackInputSignals& signals,
                      const InputEvent& ev);

void handleFileDrop(const playback_session_input::PlaybackInputView& view,
                    playback_session_input::PlaybackInputSignals& signals,
                    const std::vector<std::filesystem::path>& files);

}  // namespace playback_session_file_drop
