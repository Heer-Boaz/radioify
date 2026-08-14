#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <array>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace playback_video_edit::detail {

inline constexpr std::array<AVFrameSideDataType, 8>
    kPreservedFrameMetadata = {
        AV_FRAME_DATA_MASTERING_DISPLAY_METADATA,
        AV_FRAME_DATA_CONTENT_LIGHT_LEVEL,
        AV_FRAME_DATA_DYNAMIC_HDR_PLUS,
        AV_FRAME_DATA_DYNAMIC_HDR_VIVID,
        AV_FRAME_DATA_AMBIENT_VIEWING_ENVIRONMENT,
        AV_FRAME_DATA_ICC_PROFILE,
        AV_FRAME_DATA_A53_CC,
        AV_FRAME_DATA_DOVI_RPU_BUFFER,
};

bool isStaticMetadata(AVFrameSideDataType type);

struct MetadataFingerprint {
  std::array<std::set<uint64_t>, kPreservedFrameMetadata.size()> values;
  std::array<uint64_t, kPreservedFrameMetadata.size()> occurrences{};

  void observe(const AVFrame* frame);
};

class StaticFrameMetadata {
 public:
  void capture(AVFrameSideDataType type, const uint8_t* data, size_t size);
  void capture(const AVFrameSideData* sideData);
  void capture(const AVFrame* frame);
  void capture(const AVCodecContext* context);
  bool apply(AVFrame* frame, std::string* error) const;
  bool applyToEncoder(AVCodecContext* context) const;

 private:
  struct Entry {
    AVFrameSideDataType type;
    std::vector<uint8_t> bytes;
  };
  std::vector<Entry> entries_;
};

bool metadataSurvivedFilter(const MetadataFingerprint& source,
                            const MetadataFingerprint& filtered,
                            std::string* error);

struct DecodedVideoAudit {
  MetadataFingerprint metadata;
  AVPixelFormat pixelFormat = AV_PIX_FMT_NONE;
};

bool decodeStreamAudit(const std::filesystem::path& path, int streamIndex,
                       DecodedVideoAudit* videoAudit, std::string* error);
bool equalCriticalMetadata(const MetadataFingerprint& expected,
                           const MetadataFingerprint& actual,
                           std::string* error);

}  // namespace playback_video_edit::detail
