#include "playback/video/edit/export_dolby_vision.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>

namespace playback_video_edit::detail {
namespace {

void setError(std::string* destination, const std::string& message) {
  if (destination) *destination = message;
}

const AVPacketSideData* dolbyVisionConfigurationSideData(
    const AVCodecParameters* parameters) {
  return parameters ? av_packet_side_data_get(parameters->coded_side_data,
                                              parameters->nb_coded_side_data,
                                              AV_PKT_DATA_DOVI_CONF)
                    : nullptr;
}

const AVDOVIDecoderConfigurationRecord* dolbyVisionConfiguration(
    const AVPacketSideData* entries, int entryCount) {
  const AVPacketSideData* sideData =
      av_packet_side_data_get(entries, entryCount, AV_PKT_DATA_DOVI_CONF);
  if (!sideData || !sideData->data ||
      sideData->size < sizeof(AVDOVIDecoderConfigurationRecord)) {
    return nullptr;
  }
  return reinterpret_cast<const AVDOVIDecoderConfigurationRecord*>(
      sideData->data);
}

bool sameConfiguration(const AVDOVIDecoderConfigurationRecord& expected,
                       const AVDOVIDecoderConfigurationRecord* actual) {
  // Compare the decoding contract. Metadata compression is a bitstream
  // representation detail; FFmpeg/libx265 regenerates equivalent uncompressed
  // RPUs from the parsed metadata supplied below.
  return actual && expected.dv_version_major == actual->dv_version_major &&
         expected.dv_version_minor == actual->dv_version_minor &&
         expected.dv_profile == actual->dv_profile &&
         expected.dv_level == actual->dv_level &&
         expected.rpu_present_flag == actual->rpu_present_flag &&
         expected.el_present_flag == actual->el_present_flag &&
         expected.bl_present_flag == actual->bl_present_flag &&
         expected.dv_bl_signal_compatibility_id ==
             actual->dv_bl_signal_compatibility_id;
}

bool readHevcTierAndLevel(const AVCodecParameters* parameters, int* level,
                          int* tier) {
  if (!parameters || !level || !tier || !parameters->extradata ||
      parameters->extradata_size < 13 || parameters->extradata[0] != 1) {
    return false;
  }
  *tier = (parameters->extradata[1] & 0x20) != 0;
  *level = parameters->extradata[12];
  return parameters->level <= 0 || parameters->level == AV_LEVEL_UNKNOWN ||
         parameters->level == *level;
}

const AVCPBProperties* codedBufferingProperties(
    const AVCodecParameters* parameters) {
  if (!parameters) return nullptr;
  const AVPacketSideData* sideData = av_packet_side_data_get(
      parameters->coded_side_data, parameters->nb_coded_side_data,
      AV_PKT_DATA_CPB_PROPERTIES);
  if (!sideData || !sideData->data ||
      sideData->size < sizeof(AVCPBProperties)) {
    return nullptr;
  }
  return reinterpret_cast<const AVCPBProperties*>(sideData->data);
}

struct HevcLevelLimits {
  int level;
  int x265Level;
  int64_t mainBitrate;
  int64_t highBitrate;
};

const HevcLevelLimits* hevcLevelLimits(int level) {
  static constexpr std::array<HevcLevelLimits, 13> kLevels = {{
      {30, 10, 128'000, 0},
      {60, 20, 1'500'000, 0},
      {63, 21, 3'000'000, 0},
      {90, 30, 6'000'000, 0},
      {93, 31, 10'000'000, 0},
      {120, 40, 12'000'000, 30'000'000},
      {123, 41, 20'000'000, 50'000'000},
      {150, 50, 25'000'000, 100'000'000},
      {153, 51, 40'000'000, 160'000'000},
      {156, 52, 60'000'000, 240'000'000},
      {180, 60, 60'000'000, 240'000'000},
      {183, 61, 120'000'000, 480'000'000},
      {186, 62, 240'000'000, 800'000'000},
  }};
  const auto found =
      std::find_if(kLevels.begin(), kLevels.end(),
                   [level](const auto& entry) { return entry.level == level; });
  return found != kLevels.end() ? &*found : nullptr;
}

}  // namespace

DolbyVisionExportContract::~DolbyVisionExportContract() {
  av_frame_side_data_free(&firstFrameSideData_, &firstFrameSideDataCount_);
}

bool DolbyVisionExportContract::inspectSource(const AVCodecParameters* source,
                                              std::string* error) {
  if (!source) {
    setError(error, "The source video stream is missing.");
    return false;
  }
  const AVPacketSideData* sideData = dolbyVisionConfigurationSideData(source);
  if (!sideData) return true;
  if (!sideData->data ||
      sideData->size < sizeof(AVDOVIDecoderConfigurationRecord)) {
    setError(error,
             "The source has an invalid Dolby Vision configuration; export "
             "was not started.");
    return false;
  }

  sourceConfiguration_ =
      *reinterpret_cast<const AVDOVIDecoderConfigurationRecord*>(
          sideData->data);
  sourceDeclared_ = true;
  sourceCodec_ = source->codec_id;
  if (sourceConfiguration_.el_present_flag) {
    setError(error,
             "This Dolby Vision source contains an enhancement layer, which "
             "the installed encoders cannot recreate; export was not "
             "started.");
    return false;
  }
  if (!sourceConfiguration_.rpu_present_flag ||
      !sourceConfiguration_.bl_present_flag) {
    setError(error,
             "This Dolby Vision source does not contain the single base-layer "
             "plus RPU form required for a loss-preserving edit; export was "
             "not started.");
    return false;
  }
  if (sourceCodec_ == AV_CODEC_ID_HEVC &&
      !readHevcTierAndLevel(source, &sourceHevcLevel_, &sourceHevcTier_)) {
    setError(error,
             "The source HEVC tier and level cannot be determined reliably "
             "from its decoder configuration; export was not started.");
    return false;
  }
  return true;
}

bool DolbyVisionExportContract::inspectFirstRetainedFrame(const AVFrame* frame,
                                                          std::string* error) {
  if (!frame) {
    setError(error, "The first retained video frame is missing.");
    return false;
  }
  const AVFrameSideData* rawRpu =
      av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_RPU_BUFFER);
  const AVFrameSideData* metadata =
      av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA);
  if (!sourceDeclared_ && (rawRpu || metadata)) {
    setError(error,
             "The source contains Dolby Vision frame data but no stream "
             "configuration describing its profile and level; export was "
             "not started.");
    return false;
  }
  if (!sourceDeclared_) return true;
  if (av_frame_get_side_data(frame,
                             AV_FRAME_DATA_AMBIENT_VIEWING_ENVIRONMENT)) {
    setError(error,
             "The installed Dolby Vision encoder cannot retain this source's "
             "H.274 ambient-viewing metadata; export was not started.");
    return false;
  }
  if (!metadata) {
    setError(error,
             "The first retained Dolby Vision frame has no parsed RPU "
             "metadata; export was not started.");
    return false;
  }
  if (firstFrameSideDataCount_ > 0) return true;
  if (av_frame_side_data_clone(&firstFrameSideData_, &firstFrameSideDataCount_,
                               metadata, AV_FRAME_SIDE_DATA_FLAG_UNIQUE) < 0) {
    setError(error, "Could not retain the first Dolby Vision frame metadata.");
    return false;
  }
  return true;
}

bool DolbyVisionExportContract::validateRenderPlan(bool hasSmoothCut,
                                                   std::string* error) const {
  if (!sourceDeclared_ || !hasSmoothCut) return true;
  setError(error,
           "Smooth cut renders new pixels and cannot preserve source Dolby "
           "Vision analysis frame-for-frame; use a hard cut for this source.");
  return false;
}

bool DolbyVisionExportContract::applyFirstFrameMetadata(
    AVCodecContext* context) const {
  if (!context) return false;
  if (!sourceDeclared_) return true;
  const AVFrameSideData* metadata =
      av_frame_side_data_get(firstFrameSideData_, firstFrameSideDataCount_,
                             AV_FRAME_DATA_DOVI_METADATA);
  return metadata &&
         av_frame_side_data_clone(&context->decoded_side_data,
                                  &context->nb_decoded_side_data, metadata,
                                  AV_FRAME_SIDE_DATA_FLAG_UNIQUE) >= 0;
}

bool DolbyVisionExportContract::configureEncoder(
    const AVCodecParameters* source, const std::string& encoderName,
    AVCodecContext* context, AVDictionary** options,
    std::string* failure) const {
  if (!sourceDeclared_) return true;
  if (!source || !context || !options || !applyFirstFrameMetadata(context)) {
    setError(failure,
             "could not accept the source Dolby Vision frame metadata");
    return false;
  }

  int64_t codecBitrateLimit = std::numeric_limits<int64_t>::max();
  int x265Level = 0;
  if (sourceCodec_ == AV_CODEC_ID_HEVC) {
    const HevcLevelLimits* limits = hevcLevelLimits(sourceHevcLevel_);
    if (!limits || (sourceHevcTier_ != 0 &&
                    (sourceHevcTier_ != 1 || limits->highBitrate == 0))) {
      setError(failure,
               "the source HEVC level and tier have no supported VBV "
               "contract");
      return false;
    }
    x265Level = limits->x265Level;
    codecBitrateLimit =
        sourceHevcTier_ ? limits->highBitrate : limits->mainBitrate;
  }

  int64_t maxBitrate = 0;
  int64_t bufferSize = 0;
  const AVCPBProperties* sourceCpb = codedBufferingProperties(source);
  if (sourceCpb && sourceCpb->max_bitrate >= 1000 &&
      sourceCpb->buffer_size >= 1000) {
    maxBitrate = sourceCpb->max_bitrate;
    bufferSize = sourceCpb->buffer_size;
    if (maxBitrate > codecBitrateLimit || bufferSize > codecBitrateLimit) {
      setError(failure,
               "the source CPB properties exceed its declared HEVC level "
               "and tier");
      return false;
    }
  } else {
    constexpr std::array<int64_t, 14> kMainTierMaxBitrateMbps = {
        0, 20, 20, 20, 20, 20, 25, 25, 40, 40, 60, 60, 120, 240};
    constexpr std::array<int64_t, 14> kHighTierMaxBitrateMbps = {
        0, 50, 50, 70, 70, 70, 130, 130, 130, 130, 240, 240, 450, 800};
    if (sourceConfiguration_.dv_level == 0 ||
        sourceConfiguration_.dv_level >= kHighTierMaxBitrateMbps.size()) {
      setError(failure,
               "the source Dolby Vision level has no defined VBV limit");
      return false;
    }
    const auto& doviLimits = sourceHevcTier_ == 1 ? kHighTierMaxBitrateMbps
                                                  : kMainTierMaxBitrateMbps;
    const int64_t doviBitrateLimit =
        doviLimits[sourceConfiguration_.dv_level] * 1'000'000;
    maxBitrate = std::min(doviBitrateLimit, codecBitrateLimit);
    // Dolby's encoder guidance uses a two-second default VBV. The HEVC level
    // still caps the CPB, so the output cannot silently raise its decoder tier.
    bufferSize = std::min(maxBitrate * 2, codecBitrateLimit);
  }

  if (maxBitrate < 1000 || bufferSize < 1000 ||
      bufferSize > std::numeric_limits<int>::max()) {
    setError(failure, "the Dolby Vision VBV contract exceeds this encoder API");
    return false;
  }
  context->rc_max_rate = maxBitrate;
  context->rc_buffer_size = static_cast<int>(bufferSize);
  if (encoderName == "libx265" && x265Level > 0) {
    const std::string parameters =
        "level-idc=" + std::to_string(x265Level) +
        ":high-tier=" + std::to_string(sourceHevcTier_);
    av_dict_set(options, "x265-params", parameters.c_str(), 0);
  }
  return true;
}

bool DolbyVisionExportContract::validateEncoder(const AVCodecContext* context,
                                                std::string* failure) const {
  if (!sourceDeclared_) return true;
  const AVDOVIDecoderConfigurationRecord* actual =
      context ? dolbyVisionConfiguration(context->coded_side_data,
                                         context->nb_coded_side_data)
              : nullptr;
  if (sameConfiguration(sourceConfiguration_, actual)) return true;
  setError(failure, "cannot retain the Dolby Vision profile and level");
  return false;
}

bool DolbyVisionExportContract::validateRenderedFrame(
    const AVFrame* frame, std::string* error) const {
  if (!sourceDeclared_) return true;
  if (frame && av_frame_get_side_data(
                   frame, AV_FRAME_DATA_AMBIENT_VIEWING_ENVIRONMENT)) {
    setError(error,
             "The installed Dolby Vision encoder cannot retain this source's "
             "H.274 ambient-viewing metadata; no output was published.");
    return false;
  }
  if (frame && av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA)) {
    return true;
  }
  setError(error,
           "The edit render graph dropped Dolby Vision frame metadata; no "
           "output was published.");
  return false;
}

bool DolbyVisionExportContract::validateOutput(const AVCodecParameters* actual,
                                               std::string* error) const {
  const AVDOVIDecoderConfigurationRecord* actualConfiguration =
      actual ? dolbyVisionConfiguration(actual->coded_side_data,
                                        actual->nb_coded_side_data)
             : nullptr;
  if (!sourceDeclared_) {
    if (!actualConfiguration) return true;
    setError(error, "The completed file unexpectedly declares Dolby Vision.");
    return false;
  }
  if (!sameConfiguration(sourceConfiguration_, actualConfiguration)) {
    setError(error,
             "The Dolby Vision profile or configuration changed; the "
             "completed file was rejected.");
    return false;
  }
  if (sourceCodec_ == AV_CODEC_ID_HEVC) {
    int actualLevel = AV_LEVEL_UNKNOWN;
    int actualTier = -1;
    if (!readHevcTierAndLevel(actual, &actualLevel, &actualTier) ||
        actualLevel != sourceHevcLevel_ || actualTier != sourceHevcTier_) {
      setError(error,
               "The HEVC level or tier changed while preserving Dolby "
               "Vision; the completed file was rejected.");
      return false;
    }
  }
  return true;
}

}  // namespace playback_video_edit::detail
