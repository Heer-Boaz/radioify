#pragma once

#include "gpu/videoprocessor.h"
#include "videodecoder.h"

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace playback_video_frame_cache {

bool update(GpuVideoFrameCache& cache, ID3D11Device* device,
            ID3D11DeviceContext* context, const VideoFrame& frame);

}  // namespace playback_video_frame_cache
