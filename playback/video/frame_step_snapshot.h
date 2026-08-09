#pragma once

#include <mutex>

#include "playback/video/decoder.h"

namespace playback_video_frame_step_snapshot {

// Materialize a decoder-backed D3D11 array slice into an independently owned
// one-slice texture. The copy is queued on the shared immediate context before
// this function returns, while the caller still owns source.hwFrameRef. That
// ordering is what permits FFmpeg to recycle the decoder-pool slot afterwards.
bool materializeHardwareFrame(const VideoFrame &source, ID3D11Device *device,
                              ID3D11DeviceContext *immediateContext,
                              std::recursive_mutex *immediateContextMutex,
                              VideoFrame *snapshot);

} // namespace playback_video_frame_step_snapshot
