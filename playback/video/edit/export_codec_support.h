#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include <vector>

namespace playback_video_edit::detail {

bool normalizedChannelLayout(const AVChannelLayout& source,
                             AVChannelLayout* destination);
const AVCodec* preferredDecoder(AVCodecID codecId);
bool samePixelGeometry(AVPixelFormat left, AVPixelFormat right);
AVPixelFormat choosePreservingPixelFormat(const AVCodec* codec,
                                          AVPixelFormat sourceFormat);
bool supportsSampleRate(const AVCodec* codec, int sampleRate);
AVSampleFormat chooseSampleFormat(const AVCodec* codec,
                                  AVSampleFormat sourceFormat);
std::vector<const AVCodec*> encoderCandidates(AVCodecID codecId);
const char* encoderProfileOption(AVCodecID codecId, int profile);

}  // namespace playback_video_edit::detail
