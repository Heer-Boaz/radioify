#pragma once

#include <string>

#include "playback/media_processing_service.h"

namespace playback_session {

std::string mediaTaskFeedback(
    const playback_media_processing::Completion& completion,
    const std::filesystem::path& identifySource = {});

}  // namespace playback_session
