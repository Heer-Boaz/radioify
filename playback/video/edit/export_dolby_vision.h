#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/frame.h>
}

#include <string>

namespace playback_video_edit::detail {

class DolbyVisionExportContract {
 public:
  DolbyVisionExportContract() = default;
  ~DolbyVisionExportContract();

  DolbyVisionExportContract(const DolbyVisionExportContract&) = delete;
  DolbyVisionExportContract& operator=(const DolbyVisionExportContract&) =
      delete;

  bool inspectSource(const AVCodecParameters* source, std::string* error);
  bool inspectFirstRetainedFrame(const AVFrame* frame, std::string* error);
  bool validateRenderPlan(bool hasSmoothCut, std::string* error) const;

  bool configureEncoder(const AVCodecParameters* source,
                        const std::string& encoderName, AVCodecContext* context,
                        AVDictionary** options, std::string* failure) const;
  bool validateEncoder(const AVCodecContext* context,
                       std::string* failure) const;
  bool validateRenderedFrame(const AVFrame* frame, std::string* error) const;
  bool validateOutput(const AVCodecParameters* actual,
                      std::string* error) const;

 private:
  bool applyFirstFrameMetadata(AVCodecContext* context) const;

  AVDOVIDecoderConfigurationRecord sourceConfiguration_{};
  AVCodecID sourceCodec_ = AV_CODEC_ID_NONE;
  int sourceHevcLevel_ = AV_LEVEL_UNKNOWN;
  int sourceHevcTier_ = -1;
  bool sourceDeclared_ = false;
  AVFrameSideData** firstFrameSideData_ = nullptr;
  int firstFrameSideDataCount_ = 0;
};

}  // namespace playback_video_edit::detail
