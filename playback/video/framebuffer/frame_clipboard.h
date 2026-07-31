#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "frame_snapshot.h"

namespace playback_video_frame_clipboard {

bool buildDibV5Payload(const VideoFrameSnapshot& snapshot,
                       std::vector<uint8_t>* payload, std::string* error);

bool copyToClipboard(HWND owner, const VideoFrameSnapshot& snapshot,
                     std::string* error);

}  // namespace playback_video_frame_clipboard
