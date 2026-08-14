#include "playback/video/edit/export.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "core/runtime_helpers.h"

namespace playback_video_edit {
namespace {

constexpr AVRational kMicrosecondTimeBase{1, AV_TIME_BASE};

std::string ffmpegError(int error) {
  char buffer[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(error, buffer, sizeof(buffer));
  return std::string(buffer);
}

void setError(std::string* destination, const std::string& message) {
  if (destination) *destination = message;
}

bool validRanges(const std::vector<SourceRange>& ranges) {
  if (ranges.empty()) return false;
  int64_t previousEnd = -1;
  for (const SourceRange& range : ranges) {
    if (range.startUs < 0 || range.endUs <= range.startUs ||
        range.startUs < previousEnd) {
      return false;
    }
    previousEnd = range.endUs;
  }
  return true;
}

std::filesystem::path temporaryOutputPath(
    const std::filesystem::path& destination) {
  static std::atomic<uint64_t> sequence{0};
  const uint64_t uniqueSequence =
      sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  const std::wstring suffix =
      L".radioify-part-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
      std::to_wstring(GetTickCount64()) + L"-" +
      std::to_wstring(uniqueSequence) + L".tmp";
  return destination.parent_path() /
         std::filesystem::path(destination.filename().wstring() + suffix);
}

struct PipelineResult {
  ExportState state = ExportState::Failed;
  std::string videoEncoder;
  std::string error;
};

struct ExportPipeline {
  AVFormatContext* input = nullptr;
  AVFormatContext* output = nullptr;
  AVCodecContext* videoDecoder = nullptr;
  AVCodecContext* audioDecoder = nullptr;
  AVCodecContext* videoEncoder = nullptr;
  AVCodecContext* audioEncoder = nullptr;
  AVFilterGraph* graph = nullptr;
  AVFilterContext* videoSource = nullptr;
  AVFilterContext* audioSource = nullptr;
  AVFilterContext* videoSink = nullptr;
  AVFilterContext* audioSink = nullptr;
  AVStream* inputVideoStream = nullptr;
  AVStream* inputAudioStream = nullptr;
  AVStream* outputVideoStream = nullptr;
  AVStream* outputAudioStream = nullptr;
  int videoStreamIndex = -1;
  int audioStreamIndex = -1;
  int64_t formatStartUs = 0;
  int64_t nextAudioPtsUs = AV_NOPTS_VALUE;
  AVPacket* packet = nullptr;
  AVFrame* decoded = nullptr;
  AVFrame* filtered = nullptr;
  std::filesystem::path temporaryPath;
  std::string selectedVideoEncoder;
  std::atomic<bool>* cancelled = nullptr;
  bool videoSourceSatisfied = false;
  bool audioSourceSatisfied = false;

  ~ExportPipeline() {
    av_packet_free(&packet);
    av_frame_free(&decoded);
    av_frame_free(&filtered);
    avfilter_graph_free(&graph);
    avcodec_free_context(&videoDecoder);
    avcodec_free_context(&audioDecoder);
    avcodec_free_context(&videoEncoder);
    avcodec_free_context(&audioEncoder);
    if (output) {
      if (!(output->oformat->flags & AVFMT_NOFILE) && output->pb) {
        avio_closep(&output->pb);
      }
      avformat_free_context(output);
    }
    avformat_close_input(&input);
    if (!temporaryPath.empty()) {
      std::error_code cleanupError;
      std::filesystem::remove(temporaryPath, cleanupError);
    }
  }

  bool isCancelled() const {
    return cancelled && cancelled->load(std::memory_order_relaxed);
  }

  static int interrupt(void* opaque) {
    const auto* self = static_cast<const ExportPipeline*>(opaque);
    return self && self->isCancelled() ? 1 : 0;
  }

  static const AVCodec* preferredDecoder(AVCodecID codecId) {
    // The default Windows AV1 decoder may resolve to Media Foundation on a
    // machine without AV1 hardware. Export owns CPU frames, so prefer the
    // established software implementations and retain the codec-id fallback.
    if (codecId == AV_CODEC_ID_AV1) {
      for (const char* name : {"libdav1d", "libaom-av1"}) {
        const AVCodec* decoder = avcodec_find_decoder_by_name(name);
        if (decoder && decoder->id == codecId) return decoder;
      }
    }
    return avcodec_find_decoder(codecId);
  }

  static bool normalizedChannelLayout(const AVChannelLayout& source,
                                      AVChannelLayout* destination) {
    if (!destination) return false;
    av_channel_layout_uninit(destination);
    const int channels = source.nb_channels > 0 ? source.nb_channels : 2;
    if (source.order != AV_CHANNEL_ORDER_UNSPEC &&
        av_channel_layout_check(&source)) {
      return av_channel_layout_copy(destination, &source) >= 0;
    }
    // Older containers commonly publish only a channel count. Encoders need
    // the corresponding semantic layout (for example one channel == mono).
    av_channel_layout_default(destination, channels);
    return av_channel_layout_check(destination) != 0;
  }

  bool openDecoder(int streamIndex, AVCodecContext** destination,
                   std::string* error) {
    AVStream* stream = input->streams[streamIndex];
    const AVCodec* codec = preferredDecoder(stream->codecpar->codec_id);
    if (!codec) {
      setError(error, "No decoder is available for " +
                          std::string(av_get_media_type_string(
                              stream->codecpar->codec_type)) +
                          ".");
      return false;
    }
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (!context) {
      setError(error, "Could not allocate a media decoder.");
      return false;
    }
    int result = avcodec_parameters_to_context(context, stream->codecpar);
    if (result >= 0) result = avcodec_open2(context, codec, nullptr);
    if (result < 0) {
      setError(error, "Could not open decoder: " + ffmpegError(result));
      avcodec_free_context(&context);
      return false;
    }
    *destination = context;
    return true;
  }

  bool selectInputStream(AVMediaType type, int requestedIndex,
                         int relatedStream, int* selectedIndex,
                         std::string* error) {
    if (!selectedIndex) return false;
    if (requestedIndex >= 0) {
      if (requestedIndex >= static_cast<int>(input->nb_streams) ||
          input->streams[requestedIndex]->codecpar->codec_type != type) {
        setError(error, std::string("The selected ") +
                            av_get_media_type_string(type) +
                            " stream is no longer available.");
        return false;
      }
      *selectedIndex = requestedIndex;
      return true;
    }
    *selectedIndex =
        av_find_best_stream(input, type, -1, relatedStream, nullptr, 0);
    return true;
  }

  bool openInput(const ExportRequest& request, std::string* error) {
    input = avformat_alloc_context();
    if (!input) {
      setError(error, "Could not allocate the input container.");
      return false;
    }
    input->interrupt_callback.callback = &ExportPipeline::interrupt;
    input->interrupt_callback.opaque = this;
    const std::string sourceUtf8 = toUtf8String(request.sourcePath);
    int result = avformat_open_input(&input, sourceUtf8.c_str(), nullptr, nullptr);
    if (result < 0) {
      setError(error, "Could not open source video: " + ffmpegError(result));
      return false;
    }
    result = avformat_find_stream_info(input, nullptr);
    if (result < 0) {
      setError(error, "Could not read source stream information: " +
                          ffmpegError(result));
      return false;
    }
    formatStartUs = input->start_time != AV_NOPTS_VALUE ? input->start_time : 0;
    if (!selectInputStream(AVMEDIA_TYPE_VIDEO, request.videoStreamIndex, -1,
                           &videoStreamIndex, error)) {
      return false;
    }
    if (videoStreamIndex < 0) {
      setError(error, "The source does not contain a video stream.");
      return false;
    }
    if (!selectInputStream(AVMEDIA_TYPE_AUDIO, request.audioStreamIndex,
                           videoStreamIndex, &audioStreamIndex, error)) {
      return false;
    }
    inputVideoStream = input->streams[videoStreamIndex];
    if (!openDecoder(videoStreamIndex, &videoDecoder, error)) return false;
    if (audioStreamIndex >= 0) {
      inputAudioStream = input->streams[audioStreamIndex];
      if (!openDecoder(audioStreamIndex, &audioDecoder, error)) return false;
    }
    return true;
  }

  static bool supportsPixelFormat(const AVCodec* codec,
                                  AVPixelFormat format) {
    if (!codec) return false;
    const void* configurations = nullptr;
    int configurationCount = 0;
    if (avcodec_get_supported_config(
            nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configurations,
            &configurationCount) < 0 ||
        !configurations) {
      return true;
    }
    const auto* formats = static_cast<const AVPixelFormat*>(configurations);
    return std::find(formats, formats + configurationCount, format) !=
           formats + configurationCount;
  }

  static AVPixelFormat choosePixelFormat(const AVCodec* codec,
                                         AVPixelFormat sourceFormat) {
    const AVPixelFormat preferred[] = {
        sourceFormat, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_P010LE,
        AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P};
    for (AVPixelFormat format : preferred) {
      if (format != AV_PIX_FMT_NONE &&
          av_pix_fmt_desc_get(format) != nullptr &&
          supportsPixelFormat(codec, format)) {
        return format;
      }
    }
    const void* configurations = nullptr;
    int configurationCount = 0;
    if (codec &&
        avcodec_get_supported_config(
            nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configurations,
            &configurationCount) >= 0 &&
        configurations && configurationCount > 0) {
      return static_cast<const AVPixelFormat*>(configurations)[0];
    }
    return AV_PIX_FMT_YUV420P;
  }

  static int alignedDimension(int value, int chromaShift) {
    const int alignment = 1 << std::max(0, chromaShift);
    return (value + alignment - 1) / alignment * alignment;
  }

  int64_t sourceVideoBitRate() const {
    int64_t bitRate = inputVideoStream->codecpar->bit_rate;
    if (bitRate <= 0 && input->bit_rate > 0) bitRate = input->bit_rate;
    if (bitRate <= 0) {
      const double fps = av_q2d(av_guess_frame_rate(
          input, inputVideoStream, nullptr));
      const double safeFps = fps > 0.0 ? fps : 30.0;
      bitRate = static_cast<int64_t>(
          videoDecoder->width * static_cast<double>(videoDecoder->height) *
          safeFps * 0.10);
    }
    return std::clamp<int64_t>(bitRate, 2'000'000, 80'000'000);
  }

  bool tryVideoEncoder(const char* name, std::string* failure) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (!codec) return false;
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (!context) return false;
    context->codec_type = AVMEDIA_TYPE_VIDEO;
    context->pix_fmt = choosePixelFormat(
        codec, static_cast<AVPixelFormat>(videoDecoder->pix_fmt));
    const AVPixFmtDescriptor* pixelDescription =
        av_pix_fmt_desc_get(context->pix_fmt);
    context->width = alignedDimension(
        videoDecoder->width,
        pixelDescription ? pixelDescription->log2_chroma_w : 0);
    context->height = alignedDimension(
        videoDecoder->height,
        pixelDescription ? pixelDescription->log2_chroma_h : 0);
    context->sample_aspect_ratio =
        inputVideoStream->sample_aspect_ratio.num > 0
            ? inputVideoStream->sample_aspect_ratio
            : videoDecoder->sample_aspect_ratio;
    context->time_base = codec->id == AV_CODEC_ID_MPEG4
                             ? AVRational{1, 60000}
                             : AVRational{1, 90000};
    context->framerate =
        av_guess_frame_rate(input, inputVideoStream, nullptr);
    if (context->framerate.num <= 0 || context->framerate.den <= 0) {
      context->framerate = AVRational{30, 1};
    }
    context->gop_size = std::max(
        12, static_cast<int>(std::ceil(av_q2d(context->framerate) * 2.0)));
    context->max_b_frames = 2;
    context->bit_rate = sourceVideoBitRate();
    context->color_range = videoDecoder->color_range;
    context->color_primaries = videoDecoder->color_primaries;
    context->color_trc = videoDecoder->color_trc;
    context->colorspace = videoDecoder->colorspace;
    context->chroma_sample_location = videoDecoder->chroma_sample_location;
    if (output->oformat->flags & AVFMT_GLOBALHEADER) {
      context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    AVDictionary* options = nullptr;
    if (std::string(name) == "libx264") {
      av_dict_set(&options, "preset", "medium", 0);
      av_dict_set(&options, "crf", "18", 0);
    } else if (std::string(name) == "h264_nvenc") {
      av_dict_set(&options, "preset", "p5", 0);
      av_dict_set(&options, "rc", "vbr", 0);
      av_dict_set(&options, "cq", "19", 0);
    }
    const int result = avcodec_open2(context, codec, &options);
    av_dict_free(&options);
    if (result < 0) {
      if (failure) {
        *failure = std::string(name) + ": " + ffmpegError(result);
      }
      avcodec_free_context(&context);
      return false;
    }
    videoEncoder = context;
    selectedVideoEncoder = name;
    return true;
  }

  bool openVideoEncoder(std::string* error) {
    std::string lastFailure;
    // Prefer a quality software encoder, then platform hardware encoders, and
    // retain FFmpeg's built-in MPEG-4 encoder as the portable final fallback.
    for (const char* name : {"libx264", "h264_nvenc", "h264_mf", "mpeg4"}) {
      if (tryVideoEncoder(name, &lastFailure)) return true;
    }
    setError(error, "No usable MP4 video encoder was found" +
                        (lastFailure.empty() ? std::string(".")
                                             : ": " + lastFailure));
    return false;
  }

  static AVSampleFormat chooseSampleFormat(const AVCodec* codec) {
    if (!codec) return AV_SAMPLE_FMT_FLTP;
    const void* configurations = nullptr;
    int configurationCount = 0;
    if (avcodec_get_supported_config(
            nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
            &configurations, &configurationCount) < 0 ||
        !configurations || configurationCount <= 0) {
      return AV_SAMPLE_FMT_FLTP;
    }
    const auto* formats = static_cast<const AVSampleFormat*>(configurations);
    const auto* planarFloat =
        std::find(formats, formats + configurationCount, AV_SAMPLE_FMT_FLTP);
    return planarFloat != formats + configurationCount ? *planarFloat
                                                        : formats[0];
  }

  static int chooseSampleRate(const AVCodec* codec, int requested) {
    if (!codec) return requested;
    const void* configurations = nullptr;
    int configurationCount = 0;
    if (avcodec_get_supported_config(
            nullptr, codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0, &configurations,
            &configurationCount) < 0 ||
        !configurations || configurationCount <= 0) {
      return requested;
    }
    const auto* rates = static_cast<const int*>(configurations);
    int selected = rates[0];
    int selectedDistance = std::abs(selected - requested);
    for (int index = 0; index < configurationCount; ++index) {
      const int distance = std::abs(rates[index] - requested);
      if (distance < selectedDistance) {
        selected = rates[index];
        selectedDistance = distance;
      }
    }
    return selected;
  }

  bool openAudioEncoder(std::string* error) {
    if (!audioDecoder) return true;
    const AVCodec* codec = avcodec_find_encoder_by_name("aac");
    if (!codec) {
      setError(error, "The FFmpeg build does not provide an AAC encoder.");
      return false;
    }
    audioEncoder = avcodec_alloc_context3(codec);
    if (!audioEncoder) {
      setError(error, "Could not allocate the audio encoder.");
      return false;
    }
    audioEncoder->sample_fmt = chooseSampleFormat(codec);
    audioEncoder->sample_rate =
        chooseSampleRate(codec, std::max(8000, audioDecoder->sample_rate));
    if (!normalizedChannelLayout(audioDecoder->ch_layout,
                                 &audioEncoder->ch_layout)) {
      setError(error, "Could not normalize the source audio layout.");
      return false;
    }
    audioEncoder->time_base = AVRational{1, audioEncoder->sample_rate};
    const int64_t channels =
        std::max<int64_t>(1, audioEncoder->ch_layout.nb_channels);
    const int64_t maximumBitRate =
        std::min<int64_t>(512000, channels * 128000);
    const int64_t sourceBitRate = inputAudioStream->codecpar->bit_rate;
    audioEncoder->bit_rate = std::clamp<int64_t>(
        sourceBitRate > 0 ? sourceBitRate : channels * 96000,
        std::min<int64_t>(64000, maximumBitRate), maximumBitRate);
    if (output->oformat->flags & AVFMT_GLOBALHEADER) {
      audioEncoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    const int result = avcodec_open2(audioEncoder, codec, nullptr);
    if (result < 0) {
      setError(error, "Could not open the AAC encoder: " +
                          ffmpegError(result));
      return false;
    }
    return true;
  }

  bool createOutput(const std::filesystem::path& path, std::string* error) {
    temporaryPath = path;
    const std::string pathUtf8 = toUtf8String(path);
    int result = avformat_alloc_output_context2(&output, nullptr, "mp4",
                                                pathUtf8.c_str());
    if (result < 0 || !output) {
      setError(error, "Could not allocate the MP4 output container: " +
                          ffmpegError(result));
      return false;
    }
    av_dict_copy(&output->metadata, input->metadata, 0);
    if (!openVideoEncoder(error) || !openAudioEncoder(error)) return false;

    outputVideoStream = avformat_new_stream(output, nullptr);
    if (!outputVideoStream) {
      setError(error, "Could not create the output video stream.");
      return false;
    }
    outputVideoStream->time_base = videoEncoder->time_base;
    outputVideoStream->avg_frame_rate = videoEncoder->framerate;
    av_dict_copy(&outputVideoStream->metadata, inputVideoStream->metadata, 0);
    result = avcodec_parameters_from_context(outputVideoStream->codecpar,
                                             videoEncoder);
    if (result < 0) {
      setError(error, "Could not publish video encoder parameters: " +
                          ffmpegError(result));
      return false;
    }

    // Rotation is display metadata, not decoded pixel content. Preserve the
    // authoritative stream matrix across the re-encode so portrait and
    // rotated sources retain the same presentation geometry.
    const AVPacketSideData* displayMatrix = av_packet_side_data_get(
        inputVideoStream->codecpar->coded_side_data,
        inputVideoStream->codecpar->nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX);
    if (displayMatrix && displayMatrix->data && displayMatrix->size > 0) {
      av_packet_side_data_remove(outputVideoStream->codecpar->coded_side_data,
                                 &outputVideoStream->codecpar->nb_coded_side_data,
                                 AV_PKT_DATA_DISPLAYMATRIX);
      AVPacketSideData* outputMatrix = av_packet_side_data_new(
          &outputVideoStream->codecpar->coded_side_data,
          &outputVideoStream->codecpar->nb_coded_side_data,
          AV_PKT_DATA_DISPLAYMATRIX, displayMatrix->size, 0);
      if (!outputMatrix) {
        setError(error, "Could not preserve the source display rotation.");
        return false;
      }
      std::memcpy(outputMatrix->data, displayMatrix->data,
                  displayMatrix->size);
    }

    if (audioEncoder) {
      outputAudioStream = avformat_new_stream(output, nullptr);
      if (!outputAudioStream) {
        setError(error, "Could not create the output audio stream.");
        return false;
      }
      outputAudioStream->time_base = audioEncoder->time_base;
      av_dict_copy(&outputAudioStream->metadata, inputAudioStream->metadata, 0);
      result = avcodec_parameters_from_context(outputAudioStream->codecpar,
                                               audioEncoder);
      if (result < 0) {
        setError(error, "Could not publish audio encoder parameters: " +
                            ffmpegError(result));
        return false;
      }
    }

    if (!(output->oformat->flags & AVFMT_NOFILE)) {
      result = avio_open(&output->pb, pathUtf8.c_str(), AVIO_FLAG_WRITE);
      if (result < 0) {
        setError(error, "Could not create the temporary export: " +
                            ffmpegError(result));
        return false;
      }
    }
    AVDictionary* muxOptions = nullptr;
    av_dict_set(&muxOptions, "movflags", "+faststart", 0);
    result = avformat_write_header(output, &muxOptions);
    av_dict_free(&muxOptions);
    if (result < 0) {
      setError(error, "Could not write the MP4 header: " +
                          ffmpegError(result));
      return false;
    }
    return true;
  }

  static AVFilterInOut* makeEndpoint(const char* label,
                                     AVFilterContext* context,
                                     AVFilterInOut* next) {
    AVFilterInOut* endpoint = avfilter_inout_alloc();
    if (!endpoint) return nullptr;
    endpoint->name = av_strdup(label);
    if (!endpoint->name) {
      avfilter_inout_free(&endpoint);
      return nullptr;
    }
    endpoint->filter_ctx = context;
    endpoint->pad_idx = 0;
    endpoint->next = next;
    return endpoint;
  }

  std::string filterDescription(const std::vector<SourceRange>& ranges,
                                std::string* error) const {
    const char* pixelFormat = av_get_pix_fmt_name(videoEncoder->pix_fmt);
    if (!pixelFormat) {
      setError(error, "The selected encoder pixel format is not named.");
      return {};
    }
    std::string description;
    description += "[vsrc]split=" + std::to_string(ranges.size());
    for (size_t i = 0; i < ranges.size(); ++i) {
      description += "[v" + std::to_string(i) + "]";
    }
    description += ";";
    if (audioEncoder) {
      description += "[asrc]asplit=" + std::to_string(ranges.size());
      for (size_t i = 0; i < ranges.size(); ++i) {
        description += "[a" + std::to_string(i) + "]";
      }
      description += ";";
    }
    for (size_t i = 0; i < ranges.size(); ++i) {
      const SourceRange& range = ranges[i];
      const std::string index = std::to_string(i);
      description += "[v" + index + "]trim=start_pts=" +
                     std::to_string(range.startUs) + ":end_pts=" +
                     std::to_string(range.endUs) +
                     ",setpts=PTS-STARTPTS[vt" + index + "];";
      if (audioEncoder) {
        const int64_t inputStartPts = av_rescale_q(
            range.startUs, kMicrosecondTimeBase,
            AVRational{1, audioDecoder->sample_rate});
        const int64_t inputEndPts = av_rescale_q(
            range.endUs, kMicrosecondTimeBase,
            AVRational{1, audioDecoder->sample_rate});
        const int64_t outputStartPts = av_rescale_q(
            range.startUs, kMicrosecondTimeBase,
            AVRational{1, audioEncoder->sample_rate});
        description += "[a" + index + "]atrim=start_pts=" +
                       std::to_string(inputStartPts) + ":end_pts=" +
                       std::to_string(inputEndPts) + ",aresample=" +
                       std::to_string(audioEncoder->sample_rate) +
                       ":async=1:first_pts=" +
                       std::to_string(outputStartPts) + ",asetpts=PTS-" +
                       std::to_string(outputStartPts) + "[at" + index +
                       "];";
      }
    }
    for (size_t i = 0; i < ranges.size(); ++i) {
      const std::string index = std::to_string(i);
      description += "[vt" + index + "]";
      if (audioEncoder) description += "[at" + index + "]";
    }
    description += "concat=n=" + std::to_string(ranges.size()) + ":v=1:a=" +
                   std::string(audioEncoder ? "1" : "0");
    if (audioEncoder) {
      description += "[vcat][acat];";
    } else {
      description += "[vcat];";
    }
    description += "[vcat]";
    if (videoEncoder->width != videoDecoder->width ||
        videoEncoder->height != videoDecoder->height) {
      description += "pad=" + std::to_string(videoEncoder->width) + ":" +
                     std::to_string(videoEncoder->height) +
                     ":0:0:color=black,";
    }
    description += "format=pix_fmts=" + std::string(pixelFormat) + "[vout]";
    if (audioEncoder) {
      const char* sampleFormat =
          av_get_sample_fmt_name(audioEncoder->sample_fmt);
      char channelLayout[128]{};
      av_channel_layout_describe(&audioEncoder->ch_layout, channelLayout,
                                 sizeof(channelLayout));
      if (!sampleFormat || channelLayout[0] == '\0') {
        setError(error, "The selected audio format cannot be described.");
        return {};
      }
      int64_t outputDurationUs = 0;
      for (const SourceRange& range : ranges) {
        outputDurationUs += range.durationUs();
      }
      const int64_t outputSamples = av_rescale_q(
          outputDurationUs, kMicrosecondTimeBase,
          AVRational{1, audioEncoder->sample_rate});
      description += ";[acat]apad=whole_len=" +
                     std::to_string(outputSamples) +
                     ",atrim=end_sample=" + std::to_string(outputSamples) +
                     ",aformat=sample_fmts=" +
                     std::string(sampleFormat) + ":sample_rates=" +
                     std::to_string(audioEncoder->sample_rate) +
                     ":channel_layouts=" + std::string(channelLayout);
      if (audioEncoder->frame_size > 0) {
        description += ",asetnsamples=n=" +
                       std::to_string(audioEncoder->frame_size) + ":p=1";
      }
      description += "[aout]";
    }
    return description;
  }

  bool createFilterGraph(const std::vector<SourceRange>& ranges,
                         std::string* error) {
    graph = avfilter_graph_alloc();
    if (!graph) {
      setError(error, "Could not allocate the edit filter graph.");
      return false;
    }
    const AVFilter* buffer = avfilter_get_by_name("buffer");
    const AVFilter* bufferSink = avfilter_get_by_name("buffersink");
    if (!buffer || !bufferSink) {
      setError(error, "The FFmpeg video filter components are unavailable.");
      return false;
    }
    AVRational aspect = inputVideoStream->sample_aspect_ratio;
    if (aspect.num <= 0 || aspect.den <= 0) aspect = AVRational{1, 1};
    char videoArgs[512];
    std::snprintf(videoArgs, sizeof(videoArgs),
                   "video_size=%dx%d:pix_fmt=%d:time_base=1/%d:"
                   "pixel_aspect=%d/%d:colorspace=%d:range=%d",
                   videoDecoder->width, videoDecoder->height,
                   videoDecoder->pix_fmt, AV_TIME_BASE, aspect.num, aspect.den,
                   videoDecoder->colorspace, videoDecoder->color_range);
    int result = avfilter_graph_create_filter(
        &videoSource, buffer, "radioify_video_source", videoArgs, nullptr,
        graph);
    if (result >= 0) {
      result = avfilter_graph_create_filter(&videoSink, bufferSink,
                                            "radioify_video_sink", nullptr,
                                            nullptr, graph);
    }
    if (result < 0) {
      setError(error, "Could not configure video edit filters: " +
                          ffmpegError(result));
      return false;
    }

    if (audioEncoder) {
      const AVFilter* audioBuffer = avfilter_get_by_name("abuffer");
      const AVFilter* audioBufferSink = avfilter_get_by_name("abuffersink");
      if (!audioBuffer || !audioBufferSink) {
        setError(error, "The FFmpeg audio filter components are unavailable.");
        return false;
      }
      const char* sampleFormat = av_get_sample_fmt_name(audioDecoder->sample_fmt);
      AVChannelLayout inputLayout{};
      if (!normalizedChannelLayout(audioDecoder->ch_layout, &inputLayout)) {
        setError(error, "Could not normalize the source audio layout.");
        return false;
      }
      char layout[128]{};
      av_channel_layout_describe(&inputLayout, layout, sizeof(layout));
      av_channel_layout_uninit(&inputLayout);
      if (!sampleFormat || layout[0] == '\0') {
        setError(error, "The source audio format cannot be described.");
        return false;
      }
      char audioArgs[512];
      std::snprintf(audioArgs, sizeof(audioArgs),
                    "time_base=1/%d:sample_rate=%d:sample_fmt=%s:"
                    "channel_layout=%s",
                    audioDecoder->sample_rate, audioDecoder->sample_rate,
                    sampleFormat,
                    layout);
      result = avfilter_graph_create_filter(
          &audioSource, audioBuffer, "radioify_audio_source", audioArgs,
          nullptr, graph);
      if (result >= 0) {
        result = avfilter_graph_create_filter(
            &audioSink, audioBufferSink, "radioify_audio_sink", nullptr,
            nullptr, graph);
      }
      if (result < 0) {
        setError(error, "Could not configure audio edit filters: " +
                            ffmpegError(result));
        return false;
      }
    }

    AVFilterInOut* graphInputs = makeEndpoint("vout", videoSink, nullptr);
    AVFilterInOut* graphOutputs = makeEndpoint("vsrc", videoSource, nullptr);
    if (audioEncoder) {
      graphInputs = makeEndpoint("aout", audioSink, graphInputs);
      graphOutputs = makeEndpoint("asrc", audioSource, graphOutputs);
    }
    if (!graphInputs || !graphOutputs) {
      avfilter_inout_free(&graphInputs);
      avfilter_inout_free(&graphOutputs);
      setError(error, "Could not allocate edit filter endpoints.");
      return false;
    }
    const std::string description = filterDescription(ranges, error);
    if (description.empty()) {
      avfilter_inout_free(&graphInputs);
      avfilter_inout_free(&graphOutputs);
      return false;
    }
    result = avfilter_graph_parse_ptr(graph, description.c_str(), &graphInputs,
                                      &graphOutputs, nullptr);
    avfilter_inout_free(&graphInputs);
    avfilter_inout_free(&graphOutputs);
    if (result >= 0) result = avfilter_graph_config(graph, nullptr);
    if (result < 0) {
      setError(error, "Could not build the edit decision graph: " +
                          ffmpegError(result));
      return false;
    }
    return true;
  }

  bool allocateWorkBuffers(std::string* error) {
    packet = av_packet_alloc();
    decoded = av_frame_alloc();
    filtered = av_frame_alloc();
    if (!packet || !decoded || !filtered) {
      setError(error, "Could not allocate export frame buffers.");
      return false;
    }
    return true;
  }

  bool writeEncoderPackets(AVCodecContext* encoder, AVStream* stream,
                           std::string* error) {
    AVPacket* encoded = av_packet_alloc();
    if (!encoded) {
      setError(error, "Could not allocate an encoded packet.");
      return false;
    }
    bool ok = true;
    for (;;) {
      const int result = avcodec_receive_packet(encoder, encoded);
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
      if (result < 0) {
        setError(error, "Encoding failed: " + ffmpegError(result));
        ok = false;
        break;
      }
      av_packet_rescale_ts(encoded, encoder->time_base, stream->time_base);
      encoded->stream_index = stream->index;
      const int writeResult = av_interleaved_write_frame(output, encoded);
      av_packet_unref(encoded);
      if (writeResult < 0) {
        setError(error, "Could not write encoded media: " +
                            ffmpegError(writeResult));
        ok = false;
        break;
      }
    }
    av_packet_free(&encoded);
    return ok;
  }

  bool encodeFrame(AVFrame* frame, AVCodecContext* encoder, AVStream* stream,
                   AVRational sourceTimeBase, std::string* error) {
    if (frame) {
      frame->pts = av_rescale_q(frame->pts, sourceTimeBase, encoder->time_base);
      if (frame->duration > 0) {
        frame->duration =
            av_rescale_q(frame->duration, sourceTimeBase, encoder->time_base);
      }
      frame->time_base = encoder->time_base;
      if (encoder->codec_type == AVMEDIA_TYPE_VIDEO) {
        frame->pict_type = AV_PICTURE_TYPE_NONE;
      }
    }
    int result = avcodec_send_frame(encoder, frame);
    if (result == AVERROR(EAGAIN)) {
      if (!writeEncoderPackets(encoder, stream, error)) return false;
      result = avcodec_send_frame(encoder, frame);
    }
    if (result < 0 && result != AVERROR_EOF) {
      setError(error, "Could not submit a frame to the encoder: " +
                          ffmpegError(result));
      return false;
    }
    return writeEncoderPackets(encoder, stream, error);
  }

  enum class SinkState { NeedInput, Produced, End, Failed };

  SinkState drainSink(AVFilterContext* sink, AVCodecContext* encoder,
                      AVStream* stream, std::string* error) {
    bool produced = false;
    for (;;) {
      av_frame_unref(filtered);
      const int result = av_buffersink_get_frame(sink, filtered);
      if (result == AVERROR(EAGAIN)) {
        return produced ? SinkState::Produced : SinkState::NeedInput;
      }
      if (result == AVERROR_EOF) return SinkState::End;
      if (result < 0) {
        setError(error, "The edit filter graph failed: " +
                            ffmpegError(result));
        return SinkState::Failed;
      }
      const AVRational timeBase = av_buffersink_get_time_base(sink);
      if (!encodeFrame(filtered, encoder, stream, timeBase, error)) {
        return SinkState::Failed;
      }
      produced = true;
    }
  }

  bool drainAvailable(std::string* error) {
    if (drainSink(videoSink, videoEncoder, outputVideoStream, error) ==
        SinkState::Failed) {
      return false;
    }
    if (audioSink &&
        drainSink(audioSink, audioEncoder, outputAudioStream, error) ==
            SinkState::Failed) {
      return false;
    }
    return true;
  }

  int64_t relativeFramePtsUs(const AVFrame* frame, AVStream* stream,
                             bool audio) {
    int64_t timestamp = frame->best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE) timestamp = frame->pts;
    if (timestamp != AV_NOPTS_VALUE) {
      const int64_t absoluteUs =
          av_rescale_q(timestamp, stream->time_base, kMicrosecondTimeBase);
      return std::max<int64_t>(0, absoluteUs - formatStartUs);
    }
    if (audio && nextAudioPtsUs != AV_NOPTS_VALUE) return nextAudioPtsUs;
    if (stream->start_time != AV_NOPTS_VALUE) {
      return std::max<int64_t>(
          0, av_rescale_q(stream->start_time, stream->time_base,
                          kMicrosecondTimeBase) -
                 formatStartUs);
    }
    return 0;
  }

  bool submitDecodedFrame(AVFrame* frame, bool audio,
                          const std::function<void(int64_t)>& progress,
                          std::string* error) {
    bool& sourceSatisfied =
        audio ? audioSourceSatisfied : videoSourceSatisfied;
    if (sourceSatisfied) return true;
    AVStream* stream = audio ? inputAudioStream : inputVideoStream;
    AVFilterContext* source = audio ? audioSource : videoSource;
    const int64_t ptsUs = relativeFramePtsUs(frame, stream, audio);
    if (audio) {
      const int sampleRate = std::max(1, audioDecoder->sample_rate);
      frame->pts = av_rescale_q(ptsUs, kMicrosecondTimeBase,
                                AVRational{1, sampleRate});
      frame->time_base = AVRational{1, sampleRate};
      frame->duration = frame->nb_samples;
      nextAudioPtsUs = ptsUs +
                       av_rescale_q(frame->nb_samples,
                                    AVRational{1, sampleRate},
                                    kMicrosecondTimeBase);
    } else if (frame->duration > 0) {
      frame->pts = ptsUs;
      frame->time_base = kMicrosecondTimeBase;
      frame->duration = av_rescale_q(frame->duration, stream->time_base,
                                     kMicrosecondTimeBase);
    } else {
      frame->pts = ptsUs;
      frame->time_base = kMicrosecondTimeBase;
    }
    const int result = av_buffersrc_add_frame_flags(
        source, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (result == AVERROR_EOF) {
      // trim/concat deliberately closes an input once every kept interval for
      // that media type has been satisfied. No later source frame can affect
      // the sequence, so this is completion rather than corruption.
      sourceSatisfied = true;
      return true;
    }
    if (result < 0) {
      setError(error, std::string("Could not feed the ") +
                          (audio ? "audio" : "video") +
                          " edit filter graph: " +
                          ffmpegError(result));
      return false;
    }
    if (progress) progress(ptsUs);
    return drainAvailable(error);
  }

  bool receiveDecoderFrames(AVCodecContext* decoder, bool audio,
                            const std::function<void(int64_t)>& progress,
                            std::string* error) {
    for (;;) {
      av_frame_unref(decoded);
      const int result = avcodec_receive_frame(decoder, decoded);
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
      if (result < 0) {
        setError(error, "Decoding failed during export: " +
                            ffmpegError(result));
        return false;
      }
      if (!submitDecodedFrame(decoded, audio, progress, error)) return false;
      if (isCancelled()) return false;
    }
  }

  bool submitPacket(AVCodecContext* decoder, AVPacket* inputPacket, bool audio,
                    const std::function<void(int64_t)>& progress,
                    std::string* error) {
    int result = avcodec_send_packet(decoder, inputPacket);
    if (result == AVERROR(EAGAIN)) {
      if (!receiveDecoderFrames(decoder, audio, progress, error)) return false;
      result = avcodec_send_packet(decoder, inputPacket);
    }
    if (result < 0 && result != AVERROR_EOF) {
      setError(error, std::string("Could not submit source ") +
                          (audio ? "audio" : "video") +
                          " packets to the decoder: " + ffmpegError(result));
      return false;
    }
    return receiveDecoderFrames(decoder, audio, progress, error);
  }

  bool closeFilterSources(std::string* error) {
    int result = videoSourceSatisfied
                     ? 0
                     : av_buffersrc_add_frame_flags(videoSource, nullptr, 0);
    if (result < 0 && result != AVERROR_EOF) {
      setError(error, "Could not finish the video edit filters: " +
                          ffmpegError(result));
      return false;
    }
    if (audioSource) {
      result = audioSourceSatisfied
                   ? 0
                   : av_buffersrc_add_frame_flags(audioSource, nullptr, 0);
      if (result < 0 && result != AVERROR_EOF) {
        setError(error, "Could not finish the audio edit filters: " +
                            ffmpegError(result));
        return false;
      }
    }
    return true;
  }

  bool drainGraphToEnd(std::string* error) {
    bool videoEnded = false;
    bool audioEnded = audioSink == nullptr;
    while (!videoEnded || !audioEnded) {
      bool madeProgress = false;
      if (!videoEnded) {
        const SinkState state =
            drainSink(videoSink, videoEncoder, outputVideoStream, error);
        if (state == SinkState::Failed) return false;
        videoEnded = state == SinkState::End;
        madeProgress = madeProgress || state == SinkState::Produced || videoEnded;
      }
      if (!audioEnded) {
        const SinkState state =
            drainSink(audioSink, audioEncoder, outputAudioStream, error);
        if (state == SinkState::Failed) return false;
        audioEnded = state == SinkState::End;
        madeProgress = madeProgress || state == SinkState::Produced || audioEnded;
      }
      if (!madeProgress && (!videoEnded || !audioEnded)) {
        const int result = avfilter_graph_request_oldest(graph);
        if (result == AVERROR_EOF) break;
        if (result < 0 && result != AVERROR(EAGAIN)) {
          setError(error, "Could not drain the edit filter graph: " +
                              ffmpegError(result));
          return false;
        }
      }
      if (isCancelled()) return false;
    }
    return true;
  }

  bool transcode(const std::function<void(int64_t)>& progress,
                 std::string* error) {
    while (!isCancelled()) {
      if (videoSourceSatisfied &&
          (!audioDecoder || audioSourceSatisfied)) {
        break;
      }
      av_packet_unref(packet);
      const int result = av_read_frame(input, packet);
      if (result == AVERROR_EOF) break;
      if (result < 0) {
        setError(error, "Could not read the source during export: " +
                            ffmpegError(result));
        return false;
      }
      bool ok = true;
      if (!videoSourceSatisfied && packet->stream_index == videoStreamIndex) {
        ok = submitPacket(videoDecoder, packet, false, progress, error);
      } else if (audioDecoder && !audioSourceSatisfied &&
                 packet->stream_index == audioStreamIndex) {
        ok = submitPacket(audioDecoder, packet, true, progress, error);
      }
      av_packet_unref(packet);
      if (!ok) return false;
    }
    if (isCancelled()) return false;
    if (!videoSourceSatisfied &&
        !submitPacket(videoDecoder, nullptr, false, progress, error)) {
      return false;
    }
    if (audioDecoder && !audioSourceSatisfied &&
        !submitPacket(audioDecoder, nullptr, true, progress, error)) {
      return false;
    }
    if (!closeFilterSources(error) || !drainGraphToEnd(error)) return false;
    if (!encodeFrame(nullptr, videoEncoder, outputVideoStream,
                     videoEncoder->time_base, error)) {
      return false;
    }
    if (audioEncoder &&
        !encodeFrame(nullptr, audioEncoder, outputAudioStream,
                     audioEncoder->time_base, error)) {
      return false;
    }
    const int trailerResult = av_write_trailer(output);
    if (trailerResult < 0) {
      setError(error, "Could not finalize the MP4 export: " +
                          ffmpegError(trailerResult));
      return false;
    }
    if (!(output->oformat->flags & AVFMT_NOFILE) && output->pb) {
      const int closeResult = avio_closep(&output->pb);
      if (closeResult < 0) {
        setError(error, "Could not close the MP4 export: " +
                            ffmpegError(closeResult));
        return false;
      }
    }
    return true;
  }
};

PipelineResult runPipeline(
    const ExportRequest& request, std::atomic<bool>* cancelled,
    const std::function<void(double)>& reportProgress) {
  PipelineResult result;
  if (request.sourcePath.empty() || request.destinationPath.empty() ||
      !validRanges(request.keptRanges)) {
    result.error = "The export request is incomplete.";
    return result;
  }
  std::error_code filesystemError;
  if (!std::filesystem::is_regular_file(request.sourcePath, filesystemError) ||
      filesystemError) {
    result.error = "The source video is no longer available.";
    return result;
  }
  if (std::filesystem::exists(request.destinationPath, filesystemError) &&
      !filesystemError) {
    result.error = "The selected output file already exists.";
    return result;
  }

  ExportPipeline pipeline;
  pipeline.cancelled = cancelled;
  pipeline.temporaryPath = temporaryOutputPath(request.destinationPath);
  std::filesystem::remove(pipeline.temporaryPath, filesystemError);
  filesystemError.clear();

  if (!pipeline.openInput(request, &result.error) ||
      !pipeline.createOutput(pipeline.temporaryPath, &result.error) ||
      !pipeline.createFilterGraph(request.keptRanges, &result.error) ||
      !pipeline.allocateWorkBuffers(&result.error)) {
    std::filesystem::remove(pipeline.temporaryPath, filesystemError);
    result.videoEncoder = pipeline.selectedVideoEncoder;
    if (pipeline.isCancelled()) {
      result.state = ExportState::Cancelled;
      result.error.clear();
    }
    return result;
  }
  result.videoEncoder = pipeline.selectedVideoEncoder;
  const int64_t progressEndUs = request.keptRanges.back().endUs;
  const auto progress = [&](int64_t sourceUs) {
    if (!reportProgress || progressEndUs <= 0) return;
    reportProgress(std::clamp(static_cast<double>(sourceUs) /
                                  static_cast<double>(progressEndUs),
                              0.0, 0.999));
  };
  const bool transcoded = pipeline.transcode(progress, &result.error);
  if (!transcoded || pipeline.isCancelled()) {
    std::filesystem::remove(pipeline.temporaryPath, filesystemError);
    result.state = pipeline.isCancelled() ? ExportState::Cancelled
                                          : ExportState::Failed;
    if (result.state == ExportState::Cancelled) result.error.clear();
    return result;
  }

  if (!MoveFileExW(pipeline.temporaryPath.c_str(),
                   request.destinationPath.c_str(), MOVEFILE_WRITE_THROUGH)) {
    const DWORD moveError = GetLastError();
    std::filesystem::remove(pipeline.temporaryPath, filesystemError);
    result.error = "Could not publish the completed export (Windows error " +
                   std::to_string(moveError) + ").";
    result.state = ExportState::Failed;
    return result;
  }
  if (reportProgress) reportProgress(1.0);
  result.state = ExportState::Succeeded;
  result.error.clear();
  return result;
}

}  // namespace

std::filesystem::path uniqueEditedOutputPath(
    const std::filesystem::path& sourcePath) {
  if (sourcePath.empty()) return {};
  const std::filesystem::path directory = sourcePath.parent_path();
  const std::wstring stem = sourcePath.stem().wstring();
  const std::wstring extension = L".mp4";
  std::error_code error;
  for (uint32_t index = 1; index < 100000; ++index) {
    const std::wstring suffix =
        index == 1 ? L" - edited" : L" - edited (" + std::to_wstring(index) + L")";
    const std::filesystem::path candidate =
        directory / std::filesystem::path(stem + suffix + extension);
    const bool exists = std::filesystem::exists(candidate, error);
    if (!error && !exists && candidate != sourcePath) return candidate;
    error.clear();
  }
  return {};
}

struct Exporter::Impl {
  mutable std::mutex mutex;
  std::atomic<bool> changed{false};
  std::thread worker;
  std::atomic<bool> cancelled{false};
  ExportSnapshot state;
  std::chrono::steady_clock::time_point lastProgressNotification =
      std::chrono::steady_clock::time_point::min();

  void updateProgress(double progress) {
    const auto now = std::chrono::steady_clock::now();
    bool notify = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (state.state != ExportState::Running) return;
      progress = std::clamp(progress, 0.0, 1.0);
      if (progress >= 1.0 || progress - state.progress >= 0.01 ||
          lastProgressNotification ==
              std::chrono::steady_clock::time_point::min() ||
          now - lastProgressNotification >= std::chrono::milliseconds(250)) {
        state.progress = progress;
        lastProgressNotification = now;
        notify = true;
      }
    }
    if (notify) changed.store(true, std::memory_order_release);
  }

  void run(ExportRequest request) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comResult);
    PipelineResult completed = runPipeline(
        request, &cancelled,
        [this](double progress) { updateProgress(progress); });
    if (uninitializeCom) CoUninitialize();
    {
      std::lock_guard<std::mutex> lock(mutex);
      state.state = completed.state;
      state.progress = completed.state == ExportState::Succeeded ? 1.0
                                                                 : state.progress;
      state.videoEncoder = std::move(completed.videoEncoder);
      state.error = std::move(completed.error);
    }
    changed.store(true, std::memory_order_release);
  }
};

Exporter::Exporter() : impl_(std::make_unique<Impl>()) {}

Exporter::~Exporter() { stop(); }

bool Exporter::start(ExportRequest request) {
  if (!impl_ || request.sourcePath.empty() || request.destinationPath.empty() ||
      !validRanges(request.keptRanges)) {
    return false;
  }
  std::thread previous;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state.state == ExportState::Running) return false;
    if (impl_->worker.joinable()) previous = std::move(impl_->worker);
  }
  if (previous.joinable()) previous.join();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->cancelled.store(false, std::memory_order_relaxed);
    impl_->state = ExportSnapshot{};
    impl_->state.state = ExportState::Running;
    impl_->state.destinationPath = request.destinationPath;
    impl_->state.keptRanges = request.keptRanges;
    impl_->lastProgressNotification =
        std::chrono::steady_clock::time_point::min();
    try {
      impl_->worker = std::thread(
          [implementation = impl_.get(), request = std::move(request)]() mutable {
            implementation->run(std::move(request));
          });
    } catch (...) {
      impl_->state.state = ExportState::Failed;
      impl_->state.error = "Could not start the export worker.";
      impl_->changed.store(true, std::memory_order_release);
      return false;
    }
  }
  impl_->changed.store(true, std::memory_order_release);
  return true;
}

void Exporter::cancel() {
  if (impl_) impl_->cancelled.store(true, std::memory_order_relaxed);
}

void Exporter::stop() {
  if (!impl_) return;
  impl_->cancelled.store(true, std::memory_order_relaxed);
  if (impl_->worker.joinable()) impl_->worker.join();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state.state == ExportState::Running) {
      impl_->state.state = ExportState::Cancelled;
    }
  }
  impl_->changed.store(false, std::memory_order_release);
}

ExportSnapshot Exporter::snapshot() const {
  if (!impl_) return {};
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state;
}

bool Exporter::consumeChanged() {
  return impl_ && impl_->changed.exchange(false, std::memory_order_acq_rel);
}

}  // namespace playback_video_edit
