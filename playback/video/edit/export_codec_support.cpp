#include "playback/video/edit/export_codec_support.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <string>

namespace playback_video_edit::detail {
namespace {

bool supportsPixelFormat(const AVCodec* codec, AVPixelFormat format) {
  if (!codec || format == AV_PIX_FMT_NONE) return false;
  const void* configurations = nullptr;
  int count = 0;
  if (avcodec_get_supported_config(nullptr, codec,
                                   AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                   &configurations, &count) < 0 ||
      !configurations) {
    return true;
  }
  const auto* formats = static_cast<const AVPixelFormat*>(configurations);
  return std::find(formats, formats + count, format) != formats + count;
}

}  // namespace

bool normalizedChannelLayout(const AVChannelLayout& source,
                             AVChannelLayout* destination) {
  if (!destination) return false;
  av_channel_layout_uninit(destination);
  if (source.nb_channels <= 0) return false;
  if (source.order != AV_CHANNEL_ORDER_UNSPEC &&
      av_channel_layout_check(&source)) {
    return av_channel_layout_copy(destination, &source) >= 0;
  }
  av_channel_layout_default(destination, source.nb_channels);
  return av_channel_layout_check(destination) != 0;
}

const AVCodec* preferredDecoder(AVCodecID codecId) {
  if (codecId == AV_CODEC_ID_AV1) {
    for (const char* name : {"libdav1d", "libaom-av1"}) {
      const AVCodec* decoder = avcodec_find_decoder_by_name(name);
      if (decoder && decoder->id == codecId) return decoder;
    }
  }
  return avcodec_find_decoder(codecId);
}

bool samePixelGeometry(AVPixelFormat left, AVPixelFormat right) {
  const AVPixFmtDescriptor* lhs = av_pix_fmt_desc_get(left);
  const AVPixFmtDescriptor* rhs = av_pix_fmt_desc_get(right);
  if (!lhs || !rhs || lhs->nb_components != rhs->nb_components ||
      lhs->log2_chroma_w != rhs->log2_chroma_w ||
      lhs->log2_chroma_h != rhs->log2_chroma_h ||
      ((lhs->flags ^ rhs->flags) &
       (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_ALPHA | AV_PIX_FMT_FLAG_PAL))) {
    return false;
  }
  for (int component = 0; component < lhs->nb_components; ++component) {
    if (lhs->comp[component].depth != rhs->comp[component].depth) return false;
  }
  return true;
}

AVPixelFormat choosePreservingPixelFormat(const AVCodec* codec,
                                          AVPixelFormat sourceFormat) {
  if (supportsPixelFormat(codec, sourceFormat)) return sourceFormat;
  const void* configurations = nullptr;
  int count = 0;
  if (!codec ||
      avcodec_get_supported_config(nullptr, codec,
                                   AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                   &configurations, &count) < 0 ||
      !configurations) {
    return AV_PIX_FMT_NONE;
  }
  const auto* formats = static_cast<const AVPixelFormat*>(configurations);
  for (int index = 0; index < count; ++index) {
    if (samePixelGeometry(sourceFormat, formats[index])) return formats[index];
  }
  return AV_PIX_FMT_NONE;
}

bool supportsSampleRate(const AVCodec* codec, int sampleRate) {
  const void* configurations = nullptr;
  int count = 0;
  if (!codec || avcodec_get_supported_config(nullptr, codec,
                                             AV_CODEC_CONFIG_SAMPLE_RATE, 0,
                                             &configurations, &count) < 0 ||
      !configurations) {
    return true;
  }
  const auto* rates = static_cast<const int*>(configurations);
  return std::find(rates, rates + count, sampleRate) != rates + count;
}

AVSampleFormat chooseSampleFormat(const AVCodec* codec,
                                  AVSampleFormat sourceFormat) {
  const void* configurations = nullptr;
  int count = 0;
  if (!codec ||
      avcodec_get_supported_config(nullptr, codec,
                                   AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                   &configurations, &count) < 0 ||
      !configurations || count <= 0) {
    return sourceFormat != AV_SAMPLE_FMT_NONE ? sourceFormat
                                              : AV_SAMPLE_FMT_FLTP;
  }
  const auto* formats = static_cast<const AVSampleFormat*>(configurations);
  const auto exact = std::find(formats, formats + count, sourceFormat);
  if (exact != formats + count) return sourceFormat;
  const bool planar = av_sample_fmt_is_planar(sourceFormat) != 0;
  const int bytes = av_get_bytes_per_sample(sourceFormat);
  for (int index = 0; index < count; ++index) {
    if (av_sample_fmt_is_planar(formats[index]) == planar &&
        av_get_bytes_per_sample(formats[index]) == bytes) {
      return formats[index];
    }
  }
  return formats[0];
}

std::vector<const AVCodec*> encoderCandidates(AVCodecID codecId) {
  std::vector<const AVCodec*> candidates;
  const auto addByName = [&](const char* name) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (codec && codec->id == codecId &&
        std::find(candidates.begin(), candidates.end(), codec) ==
            candidates.end()) {
      candidates.push_back(codec);
    }
  };
  switch (codecId) {
    case AV_CODEC_ID_H264:
      addByName("h264_nvenc");
      addByName("libx264");
      addByName("h264_mf");
      break;
    case AV_CODEC_ID_HEVC:
      addByName("hevc_nvenc");
      addByName("libx265");
      addByName("hevc_mf");
      break;
    case AV_CODEC_ID_AV1:
      addByName("av1_nvenc");
      addByName("libaom-av1");
      addByName("libsvtav1");
      addByName("av1_mf");
      break;
    case AV_CODEC_ID_VP9:
      addByName("libvpx-vp9");
      break;
    case AV_CODEC_ID_VP8:
      addByName("libvpx");
      break;
    case AV_CODEC_ID_OPUS:
      addByName("libopus");
      addByName("opus");
      break;
    case AV_CODEC_ID_VORBIS:
      addByName("libvorbis");
      break;
    default:
      break;
  }
  void* opaque = nullptr;
  while (const AVCodec* codec = av_codec_iterate(&opaque)) {
    if (!av_codec_is_encoder(codec) || codec->id != codecId) continue;
    if (std::find(candidates.begin(), candidates.end(), codec) ==
        candidates.end()) {
      candidates.push_back(codec);
    }
  }
  return candidates;
}

const char* encoderProfileOption(AVCodecID codecId, int profile) {
  if (codecId == AV_CODEC_ID_H264) {
    const int baseProfile = profile & ~(AV_PROFILE_H264_CONSTRAINED |
                                        AV_PROFILE_H264_INTRA);
    switch (baseProfile) {
      case AV_PROFILE_H264_BASELINE:
        return "baseline";
      case AV_PROFILE_H264_MAIN:
        return "main";
      case AV_PROFILE_H264_HIGH:
        return "high";
      case AV_PROFILE_H264_HIGH_10:
        return "high10";
      case AV_PROFILE_H264_HIGH_422:
        return "high422";
      case AV_PROFILE_H264_HIGH_444:
      case AV_PROFILE_H264_HIGH_444_PREDICTIVE:
        return "high444p";
      default:
        return nullptr;
    }
  }
  if (codecId == AV_CODEC_ID_HEVC) {
    switch (profile) {
      case AV_PROFILE_HEVC_MAIN:
        return "main";
      case AV_PROFILE_HEVC_MAIN_10:
        return "main10";
      case AV_PROFILE_HEVC_REXT:
        return "rext";
      default:
        return nullptr;
    }
  }
  return nullptr;
}

}  // namespace playback_video_edit::detail
