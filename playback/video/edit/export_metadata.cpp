#include "playback/video/edit/export_metadata.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/ambient_viewing_environment.h>
#include <libavutil/dovi_meta.h>
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

int receiveAuditFrames(AVCodecContext* decoder, AVFrame* frame,
                       const AVFormatContext* format, int streamIndex,
                       DecodedVideoAudit* videoAudit, bool* decodedFrame) {
  for (;;) {
    const int result = avcodec_receive_frame(decoder, frame);
    if (result < 0) return result;
    if (decodedFrame) *decodedFrame = true;
    observeDecodedVideoFrame(format, streamIndex, frame, videoAudit);
    av_frame_unref(frame);
  }
}

int submitAuditPacket(AVCodecContext* decoder, const AVPacket* packet,
                      AVFrame* frame, const AVFormatContext* format,
                      int streamIndex, DecodedVideoAudit* videoAudit,
                      bool* decodedFrame) {
  int result = 0;
  while ((result = avcodec_send_packet(decoder, packet)) == AVERROR(EAGAIN)) {
    result = receiveAuditFrames(decoder, frame, format, streamIndex,
                                videoAudit, decodedFrame);
    if (result != AVERROR(EAGAIN)) return result;
  }
  if (result < 0) return result;
  return receiveAuditFrames(decoder, frame, format, streamIndex, videoAudit,
                            decodedFrame);
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

void hashColorPrimaries(uint64_t* hash,
                        const AVColorPrimariesDesc& primaries) {
  hashRational(hash, primaries.wp.x);
  hashRational(hash, primaries.wp.y);
  for (const AVCIExy* primary :
       {&primaries.prim.r, &primaries.prim.g, &primaries.prim.b}) {
    hashRational(hash, primary->x);
    hashRational(hash, primary->y);
  }
}

bool hashDolbyVisionCurve(uint64_t* hash,
                          const AVDOVIReshapingCurve& curve) {
  hashInteger(hash, curve.num_pivots);
  if (curve.num_pivots < 2 ||
      curve.num_pivots > AV_DOVI_MAX_PIECES + 1) {
    return false;
  }
  for (uint8_t index = 0; index < curve.num_pivots; ++index) {
    hashInteger(hash, curve.pivots[index]);
  }
  for (uint8_t index = 0; index + 1 < curve.num_pivots; ++index) {
    hashInteger(hash, curve.mapping_idc[index]);
    if (curve.mapping_idc[index] == AV_DOVI_MAPPING_POLYNOMIAL) {
      const uint8_t order = curve.poly_order[index];
      hashInteger(hash, order);
      if (order < 1 || order > 2) {
        return false;
      }
      for (uint8_t coefficient = 0; coefficient <= order; ++coefficient) {
        hashInteger(hash, curve.poly_coef[index][coefficient]);
      }
      continue;
    }
    if (curve.mapping_idc[index] == AV_DOVI_MAPPING_MMR) {
      const uint8_t order = curve.mmr_order[index];
      hashInteger(hash, order);
      hashInteger(hash, curve.mmr_constant[index]);
      if (order < 1 || order > 3) {
        return false;
      }
      for (uint8_t component = 0; component < order; ++component) {
        for (uint8_t coefficient = 0; coefficient < 7; ++coefficient) {
          hashInteger(hash, curve.mmr_coef[index][component][coefficient]);
        }
      }
      continue;
    }
    return false;
  }
  return true;
}

uint64_t dolbyVisionExtensionHash(const AVDOVIDmData& extension) {
  uint64_t hash = 1469598103934665603ULL;
  hashInteger(&hash, extension.level);
  switch (extension.level) {
    case 1:
      hashInteger(&hash, extension.l1.min_pq);
      hashInteger(&hash, extension.l1.max_pq);
      hashInteger(&hash, extension.l1.avg_pq);
      break;
    case 2:
      hashInteger(&hash, extension.l2.target_max_pq);
      hashInteger(&hash, extension.l2.trim_slope);
      hashInteger(&hash, extension.l2.trim_offset);
      hashInteger(&hash, extension.l2.trim_power);
      hashInteger(&hash, extension.l2.trim_chroma_weight);
      hashInteger(&hash, extension.l2.trim_saturation_gain);
      hashInteger(&hash, extension.l2.ms_weight);
      break;
    case 3:
      hashInteger(&hash, extension.l3.min_pq_offset);
      hashInteger(&hash, extension.l3.max_pq_offset);
      hashInteger(&hash, extension.l3.avg_pq_offset);
      break;
    case 4:
      hashInteger(&hash, extension.l4.anchor_pq);
      hashInteger(&hash, extension.l4.anchor_power);
      break;
    case 5:
      hashInteger(&hash, extension.l5.left_offset);
      hashInteger(&hash, extension.l5.right_offset);
      hashInteger(&hash, extension.l5.top_offset);
      hashInteger(&hash, extension.l5.bottom_offset);
      break;
    case 6:
      hashInteger(&hash, extension.l6.max_luminance);
      hashInteger(&hash, extension.l6.min_luminance);
      hashInteger(&hash, extension.l6.max_cll);
      hashInteger(&hash, extension.l6.max_fall);
      break;
    case 8:
      hashInteger(&hash, extension.l8.target_display_index);
      hashInteger(&hash, extension.l8.trim_slope);
      hashInteger(&hash, extension.l8.trim_offset);
      hashInteger(&hash, extension.l8.trim_power);
      hashInteger(&hash, extension.l8.trim_chroma_weight);
      hashInteger(&hash, extension.l8.trim_saturation_gain);
      hashInteger(&hash, extension.l8.ms_weight);
      hashInteger(&hash, extension.l8.target_mid_contrast);
      hashInteger(&hash, extension.l8.clip_trim);
      for (const uint8_t value : extension.l8.saturation_vector_field) {
        hashInteger(&hash, value);
      }
      for (const uint8_t value : extension.l8.hue_vector_field) {
        hashInteger(&hash, value);
      }
      break;
    case 9:
      hashInteger(&hash, extension.l9.source_primary_index);
      hashColorPrimaries(&hash, extension.l9.source_display_primaries);
      break;
    case 10:
      hashInteger(&hash, extension.l10.target_display_index);
      hashInteger(&hash, extension.l10.target_max_pq);
      hashInteger(&hash, extension.l10.target_min_pq);
      hashInteger(&hash, extension.l10.target_primary_index);
      hashColorPrimaries(&hash, extension.l10.target_display_primaries);
      break;
    case 11:
      hashInteger(&hash, extension.l11.content_type);
      hashInteger(&hash, extension.l11.whitepoint);
      hashInteger(&hash, extension.l11.reference_mode_flag);
      hashInteger(&hash, extension.l11.sharpness);
      hashInteger(&hash, extension.l11.noise_reduction);
      hashInteger(&hash, extension.l11.mpeg_noise_reduction);
      hashInteger(&hash, extension.l11.frame_rate_conversion);
      hashInteger(&hash, extension.l11.brightness);
      hashInteger(&hash, extension.l11.color);
      break;
    case 254:
      hashInteger(&hash, extension.l254.dm_mode);
      hashInteger(&hash, extension.l254.dm_version_index);
      break;
    case 255:
      hashInteger(&hash, extension.l255.dm_run_mode);
      hashInteger(&hash, extension.l255.dm_run_version);
      for (const uint8_t value : extension.l255.dm_debug) {
        hashInteger(&hash, value);
      }
      break;
    default:
      return hashBytes(reinterpret_cast<const uint8_t*>(&extension),
                       sizeof(extension));
  }
  return hash;
}

bool objectWithin(size_t offset, size_t objectSize, size_t totalSize) {
  return offset <= totalSize && objectSize <= totalSize - offset;
}

uint64_t canonicalDolbyVisionHash(const uint8_t* data, size_t size) {
  if (!data || size < sizeof(AVDOVIMetadata)) return hashBytes(data, size);
  const auto* metadata = reinterpret_cast<const AVDOVIMetadata*>(data);
  if (!objectWithin(metadata->header_offset, sizeof(AVDOVIRpuDataHeader), size) ||
      !objectWithin(metadata->mapping_offset, sizeof(AVDOVIDataMapping), size) ||
      !objectWithin(metadata->color_offset, sizeof(AVDOVIColorMetadata), size) ||
      metadata->num_ext_blocks < 0 ||
      metadata->num_ext_blocks > AV_DOVI_MAX_EXT_BLOCKS ||
      (metadata->num_ext_blocks > 0 &&
       (metadata->ext_block_size != sizeof(AVDOVIDmData) ||
        metadata->ext_block_size >
            (size - std::min(metadata->ext_block_offset, size)) /
                static_cast<size_t>(metadata->num_ext_blocks) ||
        !objectWithin(metadata->ext_block_offset,
                      metadata->ext_block_size *
                          static_cast<size_t>(metadata->num_ext_blocks),
                      size)))) {
    return hashBytes(data, size);
  }

  uint64_t hash = 1469598103934665603ULL;
  const AVDOVIRpuDataHeader* header = av_dovi_get_header(metadata);
  hashInteger(&hash, header->rpu_type);
  hashInteger(&hash, header->rpu_format);
  hashInteger(&hash, header->vdr_rpu_profile);
  hashInteger(&hash, header->vdr_rpu_level);
  hashInteger(&hash, header->chroma_resampling_explicit_filter_flag);
  hashInteger(&hash, header->coef_data_type);
  hashInteger(&hash, header->coef_log2_denom);
  hashInteger(&hash, header->vdr_rpu_normalized_idc);
  hashInteger(&hash, header->bl_video_full_range_flag);
  hashInteger(&hash, header->bl_bit_depth);
  hashInteger(&hash, header->el_bit_depth);
  hashInteger(&hash, header->vdr_bit_depth);
  hashInteger(&hash, header->spatial_resampling_filter_flag);
  hashInteger(&hash, header->el_spatial_resampling_filter_flag);
  hashInteger(&hash, header->disable_residual_flag);
  hashInteger(&hash, header->ext_mapping_idc_0_4);
  hashInteger(&hash, header->ext_mapping_idc_5_7);

  const AVDOVIDataMapping* mapping = av_dovi_get_mapping(metadata);
  hashInteger(&hash, mapping->vdr_rpu_id);
  hashInteger(&hash, mapping->mapping_color_space);
  hashInteger(&hash, mapping->mapping_chroma_format_idc);
  for (const AVDOVIReshapingCurve& curve : mapping->curves) {
    if (!hashDolbyVisionCurve(&hash, curve)) return hashBytes(data, size);
  }
  hashInteger(&hash, mapping->nlq_method_idc);
  hashInteger(&hash, mapping->num_x_partitions);
  hashInteger(&hash, mapping->num_y_partitions);
  if (mapping->nlq_method_idc != AV_DOVI_NLQ_NONE) {
    hashInteger(&hash, mapping->nlq_pivots[0]);
    hashInteger(&hash, mapping->nlq_pivots[1]);
    for (const AVDOVINLQParams& parameters : mapping->nlq) {
      hashInteger(&hash, parameters.nlq_offset);
      hashInteger(&hash, parameters.vdr_in_max);
      hashInteger(&hash, parameters.linear_deadzone_slope);
      hashInteger(&hash, parameters.linear_deadzone_threshold);
    }
  }

  const AVDOVIColorMetadata* color = av_dovi_get_color(metadata);
  hashInteger(&hash, color->dm_metadata_id);
  hashInteger(&hash, color->scene_refresh_flag);
  for (const AVRational value : color->ycc_to_rgb_matrix) {
    hashRational(&hash, value);
  }
  for (const AVRational value : color->ycc_to_rgb_offset) {
    hashRational(&hash, value);
  }
  for (const AVRational value : color->rgb_to_lms_matrix) {
    hashRational(&hash, value);
  }
  hashInteger(&hash, color->signal_eotf);
  hashInteger(&hash, color->signal_eotf_param0);
  hashInteger(&hash, color->signal_eotf_param1);
  hashInteger(&hash, color->signal_eotf_param2);
  hashInteger(&hash, color->signal_bit_depth);
  hashInteger(&hash, color->signal_color_space);
  hashInteger(&hash, color->signal_chroma_format);
  hashInteger(&hash, color->signal_full_range_flag);
  hashInteger(&hash, color->source_min_pq);
  hashInteger(&hash, color->source_max_pq);
  hashInteger(&hash, color->source_diagonal);

  std::vector<uint64_t> extensionHashes;
  extensionHashes.reserve(metadata->num_ext_blocks);
  for (int index = 0; index < metadata->num_ext_blocks; ++index) {
    extensionHashes.push_back(
        dolbyVisionExtensionHash(*av_dovi_get_ext(metadata, index)));
  }
  // FFmpeg may move repeated static extension blocks while regenerating an
  // RPU. Their semantic multiset is stable even when their serialized order is
  // not.
  std::sort(extensionHashes.begin(), extensionHashes.end());
  hashInteger(&hash, metadata->num_ext_blocks);
  for (const uint64_t extensionHash : extensionHashes) {
    hashInteger(&hash, static_cast<int64_t>(extensionHash));
  }
  return hash;
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
  if (type == AV_FRAME_DATA_DOVI_METADATA) {
    return canonicalDolbyVisionHash(data, size);
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
  if (frameCount == 0) frameSequences.fill(1469598103934665603ULL);
  for (size_t index = 0; index < kPreservedFrameMetadata.size(); ++index) {
    const AVFrameSideData* sideData =
        av_frame_get_side_data(frame, kPreservedFrameMetadata[index]);
    const bool present = sideData && sideData->data && sideData->size > 0;
    hashInteger(&frameSequences[index], present ? 1 : 0);
    if (!present) continue;
    const uint64_t metadataHash = canonicalMetadataHash(
        kPreservedFrameMetadata[index], sideData->data, sideData->size);
    values[index].insert(metadataHash);
    hashInteger(&frameSequences[index], static_cast<int64_t>(metadataHash));
  }
  ++frameCount;
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
        (source.frameCount != filtered.frameCount ||
         source.frameSequences[index] != filtered.frameSequences[index])) {
      setError(error, std::string("The render graph did not retain ") +
                          av_frame_side_data_name(type) +
                          " frame-for-frame; no output was published.");
      return false;
    }
  }
  return true;
}

bool validateDynamicHdrFrameForRender(const AVFrame* frame,
                                      bool rendersSyntheticFrames,
                                      std::string* error) {
  if (!frame || !rendersSyntheticFrames) return true;
  for (const AVFrameSideDataType type : {AV_FRAME_DATA_DYNAMIC_HDR_PLUS,
                                         AV_FRAME_DATA_DYNAMIC_HDR_VIVID}) {
    if (!av_frame_get_side_data(frame, type)) continue;
    setError(error, std::string("Smooth cut renders new pixels and cannot ") +
                        "preserve source " + av_frame_side_data_name(type) +
                        " analysis frame-for-frame; use a hard cut for this "
                        "source.");
    return false;
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
  AVMediaType mediaType = AVMEDIA_TYPE_UNKNOWN;
  AVCodecID codecId = AV_CODEC_ID_NONE;
  const char* failureStage = "opening the completed export";
  const std::string pathUtf8 = toUtf8String(path);
  int result = avformat_open_input(&format, pathUtf8.c_str(), nullptr, nullptr);
  if (result < 0 || !format) {
    setError(error, "Could not reopen the completed export for decode audit.");
    goto cleanup;
  }
  failureStage = "reading the completed stream table";
  result = avformat_find_stream_info(format, nullptr);
  if (result < 0 || streamIndex < 0 ||
      streamIndex >= static_cast<int>(format->nb_streams)) {
    setError(error, "Could not locate an exported stream for decode audit.");
    goto cleanup;
  }
  {
    AVStream* stream = format->streams[streamIndex];
    mediaType = stream->codecpar->codec_type;
    codecId = stream->codecpar->codec_id;
    const AVCodec* codec = preferredDecoder(stream->codecpar->codec_id);
    if (!codec) {
      setError(error, "Could not decode a completed export stream.");
      goto cleanup;
    }
    decoder = avcodec_alloc_context3(codec);
    if (!decoder) goto cleanup;
    failureStage = "opening its decoder";
    result = avcodec_parameters_to_context(decoder, stream->codecpar);
    if (result >= 0) decoder->pkt_timebase = stream->time_base;
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
  failureStage = "reading its packets";
  while ((result = av_read_frame(format, packet)) >= 0) {
    if (packet->stream_index != streamIndex) {
      av_packet_unref(packet);
      continue;
    }
    failureStage = "decoding a packet";
    result = submitAuditPacket(decoder, packet, frame, format, streamIndex,
                               videoAudit, &decodedFrame);
    av_packet_unref(packet);
    if (result != AVERROR(EAGAIN)) goto cleanup;
    failureStage = "reading its packets";
  }
  if (result != AVERROR_EOF) goto cleanup;
  failureStage = "draining its decoder";
  result = submitAuditPacket(decoder, nullptr, frame, format, streamIndex,
                             videoAudit, &decodedFrame);
  if (result != AVERROR_EOF) goto cleanup;
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
    const char* typeName = av_get_media_type_string(mediaType);
    setError(error, "The exported " +
                        std::string(typeName ? typeName : "unknown") +
                        " stream " + std::to_string(streamIndex) + " (" +
                        avcodec_get_name(codecId) + ") failed while " +
                        failureStage + ": " + ffmpegError(result));
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
        (expected.frameCount != actual.frameCount ||
         expected.frameSequences[index] != actual.frameSequences[index])) {
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
