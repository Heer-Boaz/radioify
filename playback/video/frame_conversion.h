#pragma once

#include "playback/video/image.h"

struct VideoFrame;

namespace playback_video_frame_conversion {

// Converts a decoder-owned CPU frame into the canonical RGBA8 image boundary.
// Feature-specific dimensions and cache budgets belong to the caller.
bool toRgba(const VideoFrame& frame, playback_video_image::RgbaImage* image);

}  // namespace playback_video_frame_conversion
