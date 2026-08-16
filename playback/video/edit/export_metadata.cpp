#include "playback/video/edit/export_metadata.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/ambient_viewing_environment.h>
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/mathematics.h>
#include <libavutil/mastering_display_metadata.h>
}

#include <algorithm>
#include <cstring>
#include <limits>

#include "core/runtime_helpers.h"
#include "playback/video/edit/export_codec_support.h"

namespace playback_video_edit::detail {
namespace {

void setError(std::string* destination, const std::string& message) {
  if (destination) *destination = message;
}

std::string ffmpegError(int error) {
  char buffer[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(error, buffer, sizeof(buffer));
  return std::string(buffer);
}

void observeDecodedVideoFrame(const AVFormatContext* format, int streamIndex,
                              const AVFrame* frame,
                              DecodedVideoAudit* audit) {
  if (!format || streamIndex < 0 ||
      streamIndex >= static_cast<int>(format->nb_streams) || !frame ||
      !audit) {
    return;
  }
  if (audit->pixelFormat == AV_PIX_FMT_NONE) {
    audit->pixelFormat = static_cast<AVPixelFormat>(frame->format);
  }
  audit->metadata.observe(frame);
  int64_t timestamp = frame->best_effort_timestamp;
  if (timestamp == AV_NOPTS_VALUE) timestamp = frame->pts;
  if (timestamp != AV_NOPTS_VALUE) {
    audit->cadence.observe(timestamp, format->streams[streamIndex]->time_base);
  }
}

uint64_t hashBytes(const uint8_t* data, size_t size) {
  uint64_t hash = 1469598103934665603ULL;
  for (size_t index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= 1099511628211ULL;
  }
  return hash;
}

void hashInteger(uint64_t* hash, int64_t value) {
  if (!hash) return;
  for (size_t byte = 0; byte < sizeof(value); ++byte) {
    *hash ^= static_cast<uint8_t>(
        static_cast<uint64_t>(value) >> (byte * 8));
    *hash *= 1099511628211ULL;
  }
}

void hashRational(uint64_t* hash, AVRational value) {
  int numerator = 0;
  int denominator = 1;
  av_reduce(&numerator, &denominator, value.num, value.den,
            std::numeric_limits<int>::max());
  hashInteger(hash, numerator);
  hashInteger(hash, denominator);
}

uint64_t canonicalMetadataHash(AVFrameSideDataType type, const uint8_t* data,
                               size_t size) {
  uint64_t hash = 1469598103934665603ULL;
  if (type == AV_FRAME_DATA_MASTERING_DISPLAY_METADATA &&
      size >= sizeof(AVMasteringDisplayMetadata)) {
    const auto* metadata =
        reinterpret_cast<const AVMasteringDisplayMetadata*>(data);
    for (const auto& primary : metadata->display_primaries) {
      hashRational(&hash, primary[0]);
      hashRational(&hash, primary[1]);
    }
    hashRational(&hash, metadata->white_point[0]);
    hashRational(&hash, metadata->white_point[1]);
    hashRational(&hash, metadata->min_luminance);
    hashRational(&hash, metadata->max_luminance);
    hashInteger(&hash, metadata->has_primaries);
    hashInteger(&hash, metadata->has_luminance);
    return hash;
  }
  if (type == AV_FRAME_DATA_CONTENT_LIGHT_LEVEL &&
      size >= sizeof(AVContentLightMetadata)) {
    const auto* metadata =
        reinterpret_cast<const AVContentLightMetadata*>(data);
    hashInteger(&hash, metadata->MaxCLL);
    hashInteger(&hash, metadata->MaxFALL);
    return hash;
  }
  if (type == AV_FRAME_DATA_AMBIENT_VIEWING_ENVIRONMENT &&
      size >= sizeof(AVAmbientViewingEnvironment)) {
    const auto* metadata =
        reinterpret_cast<const AVAmbientViewingEnvironment*>(data);
    hashRational(&hash, metadata->ambient_illuminance);
    hashRational(&hash, metadata->ambient_light_x);
    hashRational(&hash, metadata->ambient_light_y);
    return hash;
  }
  if (type == AV_FRAME_DATA_DYNAMIC_HDR_PLUS &&
      size >= sizeof(AVDynamicHDRPlus)) {
    uint8_t* t35 = nullptr;
    size_t t35Size = 0;
    if (av_dynamic_hdr_plus_to_t35(
            reinterpret_cast<const AVDynamicHDRPlus*>(data), &t35,
            &t35Size) >= 0 &&
        t35) {
      hash = hashBytes(t35, t35Size);
      av_free(t35);
      return hash;
    }
    av_free(t35);
  }
  return hashBytes(data, size);
}

}  // namespace

bool isStaticMetadata(AVFrameSideDataType type) {
  return type == AV_FRAME_DATA_MASTERING_DISPLAY_METADATA ||
         type == AV_FRAME_DATA_CONTENT_LIGHT_LEVEL ||
         type == AV_FRAME_DATA_AMBIENT_VIEWING_ENVIRONMENT ||
         type == AV_FRAME_DATA_ICC_PROFILE;
}

void MetadataFingerprint::observe(const AVFrame* frame) {
  if (!frame) return;
  for (size_t index = 0; index < kPreservedFrameMetadata.size(); ++index) {
    const AVFrameSideData* sideData =
        av_frame_get_side_data(frame, kPreservedFrameMetadata[index]);
    if (!sideData || !sideData->data || sideData->size == 0) continue;
    values[index].insert(canonicalMetadataHash(
        kPreservedFrameMetadata[index], sideData->data, sideData->size));
    ++occurrences[index];
  }
}

void StaticFrameMetadata::capture(AVFrameSideDataType type,
                                  const uint8_t* data, size_t size) {
  if (!isStaticMetadata(type) || !data || size == 0) return;
  const auto found =
      std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
        return entry.type == type;
      });
  if (found == entries_.end()) {
    entries_.push_back({type, std::vector<uint8_t>(data, data + size)});
  }
}

void StaticFrameMetadata::capture(const AVFrameSideData* sideData) {
  if (sideData) capture(sideData->type, sideData->data, sideData->size);
}

void StaticFrameMetadata::capture(const AVFrame* frame) {
  if (!frame) return;
  for (int index = 0; index < frame->nb_side_data; ++index) {
    capture(frame->side_data[index]);
  }
}

void StaticFrameMetadata::capture(const AVCodecContext* context) {
  if (!context) return;
  for (int index = 0; index < context->nb_decoded_side_data; ++index) {
    capture(context->decoded_side_data[index]);
  }
}

bool StaticFrameMetadata::apply(AVFrame* frame, std::string* error) const {
  if (!frame) return true;
  for (const Entry& entry : entries_) {
    if (av_frame_get_side_data(frame, entry.type)) continue;
    AVFrameSideData* sideData =
        av_frame_new_side_data(frame, entry.type, entry.bytes.size());
    if (!sideData) {
      setError(error, "Could not preserve HDR frame metadata.");
      return false;
    }
    std::memcpy(sideData->data, entry.bytes.data(), entry.bytes.size());
  }
  return true;
}

bool StaticFrameMetadata::applyToEncoder(AVCodecContext* context) const {
  if (!context) return false;
  for (const Entry& entry : entries_) {
    if (av_frame_side_data_get(context->decoded_side_data,
                               context->nb_decoded_side_data, entry.type)) {
      continue;
    }
    AVFrameSideData* sideData = av_frame_side_data_new(
        &context->decoded_side_data, &context->nb_decoded_side_data, entry.type,
        entry.bytes.size(), 0);
    if (!sideData) return false;
    std::memcpy(sideData->data, entry.bytes.data(), entry.bytes.size());
  }
  return true;
}

bool metadataSurvivedFilter(const MetadataFingerprint& source,
                            const MetadataFingerprint& filtered,
                            std::string* error) {
  for (size_t index = 0; index < kPreservedFrameMetadata.size(); ++index) {
    const AVFrameSideDataType type = kPreservedFrameMetadata[index];
    if (source.values[index] != filtered.values[index]) {
      setError(error, std::string("The render graph changed ") +
                          av_frame_side_data_name(type) +
                          "; no output was published.");
      return false;
    }
    if (!isStaticMetadata(type) &&
        source.occurrences[index] != filtered.occurrences[index]) {
      setError(error, std::string("The render graph did not retain ") +
                          av_frame_side_data_name(type) +
                          " frame-for-frame; no output was published.");
      return false;
    }
  }
  return true;
}

bool decodeStreamAudit(const std::filesystem::path& path, int streamIndex,
                       DecodedVideoAudit* videoAudit, std::string* error) {
  AVFormatContext* format = nullptr;
  AVCodecContext* decoder = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  bool ok = false;
  bool decodedFrame = false;
  const std::string pathUtf8 = toUtf8String(path);
  int result = avformat_open_input(&format, pathUtf8.c_str(), nullptr, nullptr);
  if (result < 0 || !format) {
    setError(error, "Could not reopen the completed export for decode audit.");
    goto cleanup;
  }
  result = avformat_find_stream_info(format, nullptr);
  if (result < 0 || streamIndex < 0 ||
      streamIndex >= static_cast<int>(format->nb_streams)) {
    setError(error, "Could not locate an exported stream for decode audit.");
    goto cleanup;
  }
  {
    AVStream* stream = format->streams[streamIndex];
    const AVCodec* codec = preferredDecoder(stream->codecpar->codec_id);
    if (!codec) {
      setError(error, "Could not decode a completed export stream.");
      goto cleanup;
    }
    decoder = avcodec_alloc_context3(codec);
    if (!decoder) goto cleanup;
    result = avcodec_parameters_to_context(decoder, stream->codecpar);
    if (result >= 0) result = avcodec_open2(decoder, codec, nullptr);
    if (result < 0) {
      setError(error, "Could not open an exported stream for decode audit: " +
                          ffmpegError(result));
      goto cleanup;
    }
  }
  packet = av_packet_alloc();
  frame = av_frame_alloc();
  if (!packet || !frame) goto cleanup;
  while ((result = av_read_frame(format, packet)) >= 0) {
    if (packet->stream_index != streamIndex) {
      av_packet_unref(packet);
      continue;
    }
    result = avcodec_send_packet(decoder, packet);
    av_packet_unref(packet);
    if (result < 0 && result != AVERROR(EAGAIN)) goto cleanup;
    while ((result = avcodec_receive_frame(decoder, frame)) >= 0) {
      decodedFrame = true;
      observeDecodedVideoFrame(format, streamIndex, frame, videoAudit);
      av_frame_unref(frame);
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) goto cleanup;
  }
  if (result != AVERROR_EOF) goto cleanup;
  result = avcodec_send_packet(decoder, nullptr);
  if (result < 0 && result != AVERROR_EOF) goto cleanup;
  while ((result = avcodec_receive_frame(decoder, frame)) >= 0) {
    decodedFrame = true;
    observeDecodedVideoFrame(format, streamIndex, frame, videoAudit);
    av_frame_unref(frame);
  }
  if (result != AVERROR_EOF && result != AVERROR(EAGAIN)) goto cleanup;
  ok = decodedFrame &&
       (!videoAudit || videoAudit->pixelFormat != AV_PIX_FMT_NONE);
  if (!ok) {
    setError(error, "A completed export stream contains no decodable frame.");
  }

cleanup:
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&decoder);
  avformat_close_input(&format);
  if (!ok && error && error->empty()) {
    setError(error, "An exported stream failed its full decode audit: " +
                        ffmpegError(result));
  }
  return ok;
}

bool equalCriticalMetadata(const MetadataFingerprint& expected,
                           const MetadataFingerprint& actual,
                           std::string* error) {
  for (size_t index = 0; index < kPreservedFrameMetadata.size(); ++index) {
    const AVFrameSideDataType type = kPreservedFrameMetadata[index];
    if (expected.values[index] != actual.values[index]) {
      setError(error, std::string(av_frame_side_data_name(type)) +
                          " changed during encoding; the completed file was "
                          "rejected.");
      return false;
    }
    if (!isStaticMetadata(type) &&
        expected.occurrences[index] != actual.occurrences[index]) {
      setError(error, std::string(av_frame_side_data_name(type)) +
                          " was not retained frame-for-frame; the completed "
                          "file was rejected.");
      return false;
    }
  }
  return true;
}

void CadenceFingerprint::observe(int64_t pts, AVRational timeBase) {
  if (!valid) return;
  if (pts < 0 || timeBase.num <= 0 || timeBase.den <= 0) {
    valid = false;
    return;
  }
  if (frameCount > 0) {
    if (pts <= previousPts) {
      valid = false;
      return;
    }
    constexpr AVRational kFingerprintTimeBase{1, 1'000'000'000};
    const int64_t normalizedDelta = av_rescale_q_rnd(
        pts - previousPts, timeBase, kFingerprintTimeBase,
        static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    if (normalizedDelta <= 0) {
      valid = false;
      return;
    }
    hashInteger(&hash, normalizedDelta);
  }
  previousPts = pts;
  ++frameCount;
}

bool equalFrameCadence(const CadenceFingerprint& expected,
                       const CadenceFingerprint& actual,
                       std::string* error) {
  if (!expected.valid || !actual.valid || expected.frameCount == 0 ||
      expected.frameCount != actual.frameCount ||
      expected.hash != actual.hash) {
    setError(error,
             "The encoder or muxer changed the rendered video cadence; the "
             "completed file was rejected.");
    return false;
  }
  return true;
}

}  // namespace playback_video_edit::detail
