#include "playback/video/edit/export_pipeline.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_desc.h>
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
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/runtime_helpers.h"
#include "playback/video/composition/render_plan.h"
#include "playback/video/edit/export_codec_support.h"
#include "playback/video/edit/export_metadata.h"

namespace playback_video_edit::detail {
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

void applyVideoEncoderPolicy(const std::string& name,
                             AVCodecContext* context,
                             AVDictionary** options) {
  if (!context || !options) return;
  if (name.find("_nvenc") != std::string::npos) {
    av_dict_set(options, "preset", "p6", 0);
    av_dict_set(options, "tune", "hq", 0);
    av_dict_set(options, "rc", "vbr", 0);
    av_dict_set(options, "cq", "18", 0);
    return;
  }
  if (name == "libx264" || name == "libx265") {
    context->bit_rate = 0;
    av_dict_set(options, "preset", "medium", 0);
    av_dict_set(options, "crf", "18", 0);
    return;
  }
  if (name == "libaom-av1") {
    context->bit_rate = 0;
    av_dict_set(options, "cpu-used", "6", 0);
    av_dict_set(options, "crf", "18", 0);
    av_dict_set(options, "row-mt", "1", 0);
  }
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

int64_t editedDurationUs(const std::vector<SourceRange>& ranges) {
  int64_t duration = 0;
  for (const SourceRange& range : ranges) {
    if (range.durationUs() > std::numeric_limits<int64_t>::max() - duration) {
      return std::numeric_limits<int64_t>::max();
    }
    duration += range.durationUs();
  }
  return duration;
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

bool hasNamePart(const char* names, const char* part) {
  if (!names || !part) return false;
  const std::string haystack(names);
  size_t begin = 0;
  while (begin <= haystack.size()) {
    const size_t end = haystack.find(',', begin);
    const size_t length =
        end == std::string::npos ? haystack.size() - begin : end - begin;
    if (haystack.compare(begin, length, part) == 0) return true;
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return false;
}

class MotionTimingAudit {
 public:
  bool reset(const playback_video_composition::RenderPlan& plan,
             std::string* error) {
    observations_.clear();
    segments_.clear();
    currentSegment_ = 0;
    failed_ = false;
    toleranceUs_ = plan.timestampToleranceUs;
    observations_.reserve(plan.motionTransitions.size());
    segments_.reserve(plan.motionTransitions.size() * 2);
    for (const auto& transition : plan.motionTransitions) {
      Observation observation;
      observation.transition = transition;
      if (!playback_video_composition::motionSourceTiming(
              plan, transition, &observation.expected, error)) {
        return false;
      }
      observations_.push_back(std::move(observation));
      const size_t index = observations_.size() - 1;
      segments_.push_back({index, true});
      segments_.push_back({index, false});
    }
    return true;
  }

  bool observe(int64_t ptsUs, int64_t durationUs, std::string* error) {
    if (failed_ || segments_.empty()) return !failed_;
    while (currentSegment_ < segments_.size()) {
      const Segment& segment = segments_[currentSegment_];
      Observation& observation = observations_[segment.observationIndex];
      const std::vector<int64_t>& expected =
          segment.outgoing ? observation.expected.outgoingPtsUs
                           : observation.expected.incomingPtsUs;
      std::vector<playback_video_composition::SourceFrameTiming>& actual =
          segment.outgoing ? observation.outgoing : observation.incoming;
      if (ptsUs < expected.front() &&
          !playback_video_composition::sourceTimestampMatches(
              ptsUs, expected.front(), toleranceUs_)) {
        return true;
      }
      if (ptsUs > expected.back() &&
          !playback_video_composition::sourceTimestampMatches(
              ptsUs, expected.back(), toleranceUs_)) {
        if (actual.size() != expected.size()) return fail(observation, error);
        ++currentSegment_;
        continue;
      }
      const size_t index = actual.size();
      if (index >= expected.size() ||
          !playback_video_composition::sourceTimestampMatches(
              ptsUs, expected[index], toleranceUs_)) {
        return fail(observation, error);
      }
      actual.push_back({ptsUs, durationUs});
      return true;
    }
    return true;
  }

  bool finish(const playback_video_composition::RenderPlan& plan,
              std::string* error) {
    if (failed_) return false;
    for (const Observation& observation : observations_) {
      if (!playback_video_composition::validateMotionSourceTiming(
              plan, observation.transition, observation.outgoing,
              observation.incoming, error)) {
        failed_ = true;
        return false;
      }
    }
    return true;
  }

 private:
  struct Observation {
    playback_video_composition::MotionTransitionWindow transition;
    playback_video_composition::MotionSourceTiming expected;
    std::vector<playback_video_composition::SourceFrameTiming> outgoing;
    std::vector<playback_video_composition::SourceFrameTiming> incoming;
  };

  struct Segment {
    size_t observationIndex = 0;
    bool outgoing = false;
  };

  bool fail(const Observation& observation, std::string* error) {
    failed_ = true;
    setError(error,
             "Smooth cut " +
                 std::to_string(observation.transition.cutIndex + 1) +
                 " is not aligned to a stable source-frame cadence; use a "
                 "hard cut for this edit point.");
    return false;
  }

  std::vector<Observation> observations_;
  std::vector<Segment> segments_;
  size_t currentSegment_ = 0;
  int64_t toleranceUs_ = 0;
  bool failed_ = false;
};

struct EncodedStream {
  int inputIndex = -1;
  AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
  AVStream* inputStream = nullptr;
  AVStream* outputStream = nullptr;
  AVCodecContext* decoder = nullptr;
  AVCodecContext* encoder = nullptr;
  AVFilterGraph* graph = nullptr;
  AVFilterContext* source = nullptr;
  AVFilterContext* sink = nullptr;
  AVFrame* pendingVideoFrame = nullptr;
  AVRational pendingVideoTimeBase{0, 1};
  int64_t finalVideoFrameDurationUs = 0;
  int64_t nextAudioPtsUs = AV_NOPTS_VALUE;
  bool sourceSatisfied = false;
  std::string encoderName;

  ~EncodedStream() {
    av_frame_free(&pendingVideoFrame);
    avfilter_graph_free(&graph);
    avcodec_free_context(&decoder);
    avcodec_free_context(&encoder);
  }

  bool video() const { return type == AVMEDIA_TYPE_VIDEO; }
  bool audio() const { return type == AVMEDIA_TYPE_AUDIO; }
};

struct CopiedStream {
  int inputIndex = -1;
  AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
  AVStream* inputStream = nullptr;
  AVStream* outputStream = nullptr;
  bool attachedPicture = false;
};

class ExportPipeline {
 public:
  ExportPipeline(const ExportRequest& request, std::atomic<bool>* cancelled)
      : request_(request), cancelled_(cancelled) {}

  ~ExportPipeline() {
    av_packet_free(&packet_);
    av_frame_free(&decoded_);
    av_frame_free(&filtered_);
    encodedByInput_.clear();
    encoded_.clear();
    if (output_) {
      if (!(output_->oformat->flags & AVFMT_NOFILE) && output_->pb) {
        avio_closep(&output_->pb);
      }
      avformat_free_context(output_);
    }
    avformat_close_input(&input_);
    if (!temporaryPath_.empty()) {
      std::error_code error;
      std::filesystem::remove(temporaryPath_, error);
    }
  }

  bool prepare(std::string* error);
  bool transcode(const std::function<void(int64_t)>& progress,
                 std::string* error);
  bool validate(std::string* error);
  bool publish(std::string* error);

  bool cancelled() const {
    return cancelled_ && cancelled_->load(std::memory_order_relaxed);
  }
  const std::string& videoEncoderName() const { return videoEncoderName_; }

 private:
  static int interrupt(void* opaque) {
    const auto* self = static_cast<const ExportPipeline*>(opaque);
    return self && self->cancelled() ? 1 : 0;
  }

  bool openInput(std::string* error);
  bool buildStreamPlan(std::string* error);
  bool allocateOutput(std::string* error);
  bool openDecoder(EncodedStream* stream, std::string* error);
  bool openEncoder(EncodedStream* stream, std::string* error);
  bool tryVideoEncoder(EncodedStream* stream, const AVCodec* codec,
                       std::string* failure);
  bool tryAudioEncoder(EncodedStream* stream, const AVCodec* codec,
                       std::string* failure);
  bool createOutputStreams(std::string* error);
  bool createVideoRenderPlan(std::string* error);
  bool createEncodedOutputStream(EncodedStream* stream, std::string* error);
  bool createCopiedOutputStream(CopiedStream* stream, std::string* error);
  bool copyChapters(std::string* error);
  bool openOutputFile(std::string* error);
  bool createFilterGraph(EncodedStream* stream, std::string* error);
  std::string filterDescription(const EncodedStream& stream,
                                std::string* error) const;
  bool allocateWorkBuffers(std::string* error);
  bool submitPacket(EncodedStream* stream, AVPacket* packet,
                    const std::function<void(int64_t)>& progress,
                    std::string* error);
  bool receiveFrames(EncodedStream* stream,
                     const std::function<void(int64_t)>& progress,
                     std::string* error);
  bool submitFrame(EncodedStream* stream, AVFrame* frame,
                   const std::function<void(int64_t)>& progress,
                   std::string* error);
  bool drainAvailable(EncodedStream* stream, std::string* error);
  bool drainToEnd(EncodedStream* stream, std::string* error);
  bool queueFilteredFrame(EncodedStream* stream, AVFrame* frame,
                          AVRational sourceTimeBase, std::string* error);
  bool flushPendingVideoFrame(EncodedStream* stream, std::string* error);
  bool encodeFrame(EncodedStream* stream, AVFrame* frame,
                   AVRational sourceTimeBase, std::string* error);
  bool writeEncoderPackets(EncodedStream* stream, std::string* error);
  bool copyPacket(const CopiedStream& stream, const AVPacket* packet,
                  std::string* error);
  bool finish(std::string* error);
  int64_t relativeFramePtsUs(const EncodedStream& stream,
                             const AVFrame* frame) const;
  int64_t presentationStartForRange(size_t rangeIndex) const;
  void captureStreamStaticMetadata();
  bool probeStaticVideoMetadata(std::string* error);

  const ExportRequest& request_;
  std::atomic<bool>* cancelled_ = nullptr;
  AVFormatContext* input_ = nullptr;
  AVFormatContext* output_ = nullptr;
  int selectedVideoIndex_ = -1;
  int64_t formatStartUs_ = 0;
  size_t muxerChapterProjectionCount_ = 0;
  std::vector<std::unique_ptr<EncodedStream>> encoded_;
  std::vector<EncodedStream*> encodedByInput_;
  std::vector<CopiedStream> copied_;
  std::vector<const CopiedStream*> copiedByInput_;
  AVPacket* packet_ = nullptr;
  AVFrame* decoded_ = nullptr;
  AVFrame* filtered_ = nullptr;
  std::filesystem::path temporaryPath_;
  std::string videoEncoderName_;
  StaticFrameMetadata staticMetadata_;
  MetadataFingerprint sourceMetadata_;
  MetadataFingerprint expectedMetadata_;
  CadenceFingerprint expectedVideoCadence_;
  playback_video_composition::RenderPlan videoRenderPlan_;
  MotionTimingAudit motionTimingAudit_;
};

bool ExportPipeline::openInput(std::string* error) {
  input_ = avformat_alloc_context();
  if (!input_) {
    setError(error, "Could not allocate the source container.");
    return false;
  }
  input_->interrupt_callback.callback = &ExportPipeline::interrupt;
  input_->interrupt_callback.opaque = this;
  const std::string sourceUtf8 = toUtf8String(request_.sourcePath);
  int result = avformat_open_input(&input_, sourceUtf8.c_str(), nullptr, nullptr);
  if (result < 0) {
    setError(error, "Could not open the source video: " + ffmpegError(result));
    return false;
  }
  result = avformat_find_stream_info(input_, nullptr);
  if (result < 0) {
    setError(error, "Could not read the source stream information: " +
                        ffmpegError(result));
    return false;
  }
  formatStartUs_ =
      input_->start_time != AV_NOPTS_VALUE ? input_->start_time : 0;
  if (request_.videoStreamIndex >= 0) {
    if (request_.videoStreamIndex >= static_cast<int>(input_->nb_streams) ||
        input_->streams[request_.videoStreamIndex]->codecpar->codec_type !=
            AVMEDIA_TYPE_VIDEO) {
      setError(error, "The selected video stream is no longer available.");
      return false;
    }
    selectedVideoIndex_ = request_.videoStreamIndex;
  } else {
    selectedVideoIndex_ =
        av_find_best_stream(input_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  }
  if (selectedVideoIndex_ < 0) {
    setError(error, "The source does not contain a video stream.");
    return false;
  }
  return true;
}

bool ExportPipeline::openDecoder(EncodedStream* stream, std::string* error) {
  if (!stream || !stream->inputStream) return false;
  const AVCodec* codec =
      preferredDecoder(stream->inputStream->codecpar->codec_id);
  if (!codec) {
    setError(error, "No decoder is available for source " +
                        std::string(av_get_media_type_string(stream->type)) +
                        " codec " +
                        avcodec_get_name(stream->inputStream->codecpar->codec_id) +
                        ".");
    return false;
  }
  stream->decoder = avcodec_alloc_context3(codec);
  if (!stream->decoder) {
    setError(error, "Could not allocate a source decoder.");
    return false;
  }
  int result = avcodec_parameters_to_context(
      stream->decoder, stream->inputStream->codecpar);
  if (result >= 0) {
    stream->decoder->pkt_timebase = stream->inputStream->time_base;
  }
  if (result >= 0) result = avcodec_open2(stream->decoder, codec, nullptr);
  if (result < 0) {
    setError(error, "Could not open the " + std::string(codec->name) +
                        " decoder: " + ffmpegError(result));
    return false;
  }
  return true;
}

bool ExportPipeline::buildStreamPlan(std::string* error) {
  encoded_.clear();
  copied_.clear();
  muxerChapterProjectionCount_ = 0;
  for (unsigned index = 0; index < input_->nb_streams; ++index) {
    AVStream* inputStream = input_->streams[index];
    const AVMediaType type = inputStream->codecpar->codec_type;
    if (static_cast<int>(index) == selectedVideoIndex_ ||
        type == AVMEDIA_TYPE_AUDIO) {
      auto stream = std::make_unique<EncodedStream>();
      stream->inputIndex = static_cast<int>(index);
      stream->type = type;
      stream->inputStream = inputStream;
      if (!openDecoder(stream.get(), error)) return false;
      encoded_.push_back(std::move(stream));
      continue;
    }
    const bool attachedPicture =
        type == AVMEDIA_TYPE_VIDEO &&
        (inputStream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
    if (type == AVMEDIA_TYPE_SUBTITLE) {
      const AVCodecDescriptor* descriptor =
          avcodec_descriptor_get(inputStream->codecpar->codec_id);
      if (descriptor && (descriptor->props & AV_CODEC_PROP_BITMAP_SUB) != 0) {
        setError(error,
                 "This file contains a stateful bitmap subtitle track (" +
                     std::string(avcodec_get_name(
                         inputStream->codecpar->codec_id)) +
                     ") that cannot yet be cut without risking missing "
                     "palette or object state; export was not started.");
        return false;
      }
      copied_.push_back({static_cast<int>(index), type, inputStream, nullptr,
                         false});
      continue;
    }
    if (type == AVMEDIA_TYPE_ATTACHMENT || attachedPicture) {
      copied_.push_back({static_cast<int>(index), type, inputStream, nullptr,
                         attachedPicture});
      continue;
    }
    if (type == AVMEDIA_TYPE_VIDEO) {
      setError(error,
               "Format-preserving export cannot choose between multiple "
               "video programs yet; no video stream was discarded.");
      return false;
    }
    const AVDictionaryEntry* handler =
        av_dict_get(inputStream->metadata, "handler_name", nullptr, 0);
    const bool muxerChapterProjection =
        type == AVMEDIA_TYPE_DATA && input_->nb_chapters > 0 &&
        inputStream->codecpar->codec_id == AV_CODEC_ID_BIN_DATA &&
        inputStream->codecpar->codec_tag == MKTAG('t', 'e', 'x', 't') &&
        handler && handler->value &&
        std::string(handler->value) == "SubtitleHandler" &&
        input_->iformat &&
        (hasNamePart(input_->iformat->name, "mov") ||
         hasNamePart(input_->iformat->name, "mp4"));
    if (muxerChapterProjection) {
      // MOV/MP4 demuxing exposes the chapter table as a synthetic text data
      // track. The output muxer regenerates that projection from the retimed
      // AVChapter list; copying it as an independent stream would duplicate
      // ownership and preserve stale source timestamps.
      ++muxerChapterProjectionCount_;
      continue;
    }
    if (type == AVMEDIA_TYPE_DATA) {
      setError(error,
               "This file contains a timed data track that cannot yet be "
               "retimed safely; export was not started.");
      return false;
    }
    const char* typeName = av_get_media_type_string(type);
    setError(error,
             "This file contains a " +
                 std::string(typeName ? typeName : "unknown") +
                 " stream that cannot yet be preserved; export was not "
                 "started and no stream was discarded.");
    return false;
  }

  encodedByInput_.assign(input_->nb_streams, nullptr);
  for (const auto& stream : encoded_) {
    encodedByInput_[static_cast<size_t>(stream->inputIndex)] = stream.get();
  }
  copiedByInput_.assign(input_->nb_streams, nullptr);
  for (const CopiedStream& stream : copied_) {
    copiedByInput_[static_cast<size_t>(stream.inputIndex)] = &stream;
  }

  AVStream* video = input_->streams[selectedVideoIndex_];
  if (av_packet_side_data_get(video->codecpar->coded_side_data,
                              video->codecpar->nb_coded_side_data,
                              AV_PKT_DATA_DOVI_CONF)) {
    setError(error,
             "Dolby Vision RPU metadata cannot yet be rendered without "
             "loss; export was not started.");
    return false;
  }
  return true;
}

bool ExportPipeline::allocateOutput(std::string* error) {
  if (request_.sourcePath.extension().empty() ||
      request_.destinationPath.extension().empty()) {
    setError(error, "The source container extension cannot be preserved.");
    return false;
  }
  std::wstring sourceExtension = request_.sourcePath.extension().wstring();
  std::wstring outputExtension = request_.destinationPath.extension().wstring();
  std::transform(sourceExtension.begin(), sourceExtension.end(),
                 sourceExtension.begin(), towlower);
  std::transform(outputExtension.begin(), outputExtension.end(),
                 outputExtension.begin(), towlower);
  if (sourceExtension != outputExtension) {
    setError(error,
             "The edited file must use the same container extension as the "
             "source.");
    return false;
  }
  const std::string destinationUtf8 = toUtf8String(request_.destinationPath);
  const int result = avformat_alloc_output_context2(
      &output_, nullptr, nullptr, destinationUtf8.c_str());
  if (result < 0 || !output_) {
    setError(error, "No output muxer is available for the source container " +
                        toUtf8String(request_.sourcePath.extension()) + ".");
    return false;
  }
  av_dict_copy(&output_->metadata, input_->metadata, 0);
  return true;
}

bool ExportPipeline::tryVideoEncoder(EncodedStream* stream,
                                     const AVCodec* codec,
                                     std::string* failure) {
  if (!stream || !codec) return false;
  AVCodecContext* context = avcodec_alloc_context3(codec);
  if (!context) return false;
  const AVCodecParameters* source = stream->inputStream->codecpar;
  AVPixelFormat sourceFormat =
      static_cast<AVPixelFormat>(stream->decoder->pix_fmt);
  if (sourceFormat == AV_PIX_FMT_NONE) {
    sourceFormat = static_cast<AVPixelFormat>(source->format);
  }
  context->pix_fmt = choosePreservingPixelFormat(codec, sourceFormat);
  if (context->pix_fmt == AV_PIX_FMT_NONE) {
    if (failure) {
      *failure = std::string(codec->name) +
                 " cannot preserve the source bit depth/chroma format";
    }
    avcodec_free_context(&context);
    return false;
  }
  context->codec_type = AVMEDIA_TYPE_VIDEO;
  context->codec_id = source->codec_id;
  context->width = source->width;
  context->height = source->height;
  context->sample_aspect_ratio = source->sample_aspect_ratio.num > 0
                                     ? source->sample_aspect_ratio
                                     : stream->decoder->sample_aspect_ratio;
  context->framerate = av_guess_frame_rate(input_, stream->inputStream, nullptr);
  if (context->framerate.num <= 0 || context->framerate.den <= 0) {
    context->framerate = source->framerate.num > 0 && source->framerate.den > 0
                             ? source->framerate
                             : AVRational{0, 1};
  }
  // Preserve the demuxer's timestamp grid, which is the clock that represented
  // the source cadence in its original container.  Using 1/framerate here
  // silently turns VFR timestamps into a constant-rate grid; using the edit
  // graph's microsecond clock unconditionally can exceed codec/container clock
  // limits (notably QuickTime).  AVCodecContext::framerate remains only an
  // encoder hint.
  context->time_base =
      stream->inputStream->time_base.num > 0 &&
              stream->inputStream->time_base.den > 0
          ? stream->inputStream->time_base
          : kMicrosecondTimeBase;
  // This pipeline supplies frame durations from the render graph.  Without
  // this contract libavcodec is required to discard them and encoders fall
  // back to the nominal framerate, which is wrong for VFR and can make the
  // muxer mark the final frame as discard padding.
  context->flags |= AV_CODEC_FLAG_FRAME_DURATION;
  const double nominalFramesPerSecond = av_q2d(context->framerate);
  context->gop_size =
      nominalFramesPerSecond > 0.0 && std::isfinite(nominalFramesPerSecond)
          ? std::max(12, static_cast<int>(
                             std::ceil(nominalFramesPerSecond * 2.0)))
          : 12;
  context->max_b_frames = 2;
  context->bit_rate = source->bit_rate > 0
                          ? source->bit_rate
                          : std::max<int64_t>(2'000'000, input_->bit_rate);
  context->bits_per_raw_sample = source->bits_per_raw_sample;
  context->profile = source->profile;
  context->level = source->level;
  context->field_order = source->field_order;
  if (source->field_order != AV_FIELD_UNKNOWN &&
      source->field_order != AV_FIELD_PROGRESSIVE) {
    // Advertising the field order alone is not an encoder capability request.
    // The standard FFmpeg interlace flags make NVENC capability-check the
    // hardware and let the candidate loop fall back to a software encoder
    // when this GPU cannot retain field coding.
    context->flags |=
        AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME;
  }
  context->color_range = source->color_range;
  context->color_primaries = source->color_primaries;
  context->color_trc = source->color_trc;
  context->colorspace = source->color_space;
  context->chroma_sample_location = source->chroma_location;
  if (output_->oformat->flags & AVFMT_GLOBALHEADER) {
    context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
  if (!staticMetadata_.applyToEncoder(context)) {
    if (failure) {
      *failure = std::string(codec->name) +
                 " could not accept the source static HDR metadata";
    }
    avcodec_free_context(&context);
    return false;
  }

  AVDictionary* options = nullptr;
  const std::string name(codec->name);
  if (const char* profile =
          encoderProfileOption(source->codec_id, source->profile)) {
    av_dict_set(&options, "profile", profile, 0);
  }
  applyVideoEncoderPolicy(name, context, &options);
  const int result = avcodec_open2(context, codec, &options);
  const AVDictionaryEntry* unusedOption =
      av_dict_get(options, "", nullptr, AV_DICT_IGNORE_SUFFIX);
  const std::string unusedOptionName =
      unusedOption && unusedOption->key ? unusedOption->key : "";
  av_dict_free(&options);
  if (result < 0 || !unusedOptionName.empty() ||
      context->codec_id != source->codec_id ||
      context->width != source->width || context->height != source->height ||
      !samePixelGeometry(sourceFormat, context->pix_fmt)) {
    if (failure) {
      *failure = name + ": " +
                 (result < 0
                      ? ffmpegError(result)
                      : !unusedOptionName.empty()
                            ? "required option '" + unusedOptionName +
                                  "' was not accepted"
                            : "source geometry/depth was not retained");
    }
    avcodec_free_context(&context);
    return false;
  }
  stream->encoder = context;
  stream->encoderName = name;
  return true;
}

bool ExportPipeline::tryAudioEncoder(EncodedStream* stream,
                                     const AVCodec* codec,
                                     std::string* failure) {
  if (!stream || !codec) return false;
  const AVCodecParameters* source = stream->inputStream->codecpar;
  if (!supportsSampleRate(codec, stream->decoder->sample_rate)) {
    if (failure) {
      *failure = std::string(codec->name) +
                 " cannot preserve the source sample rate";
    }
    return false;
  }
  AVCodecContext* context = avcodec_alloc_context3(codec);
  if (!context) return false;
  context->codec_type = AVMEDIA_TYPE_AUDIO;
  context->codec_id = source->codec_id;
  context->sample_fmt = chooseSampleFormat(
      codec, static_cast<AVSampleFormat>(stream->decoder->sample_fmt));
  context->sample_rate = stream->decoder->sample_rate;
  if (!normalizedChannelLayout(stream->decoder->ch_layout,
                               &context->ch_layout)) {
    avcodec_free_context(&context);
    return false;
  }
  context->time_base = AVRational{1, context->sample_rate};
  context->bit_rate = source->bit_rate;
  context->bits_per_raw_sample = source->bits_per_raw_sample;
  context->profile = source->profile;
  context->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
  if (output_->oformat->flags & AVFMT_GLOBALHEADER) {
    context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
  const int result = avcodec_open2(context, codec, nullptr);
  AVChannelLayout sourceLayout{};
  const bool sourceLayoutValid =
      normalizedChannelLayout(source->ch_layout, &sourceLayout);
  const int expectedSampleRate =
      source->sample_rate > 0 ? source->sample_rate
                              : stream->decoder->sample_rate;
  const bool formatRetained =
      result >= 0 && context->codec_id == source->codec_id &&
      context->sample_rate == expectedSampleRate && sourceLayoutValid &&
      av_channel_layout_compare(&context->ch_layout, &sourceLayout) == 0;
  av_channel_layout_uninit(&sourceLayout);
  if (!formatRetained) {
    if (failure) {
      *failure = std::string(codec->name) + ": " +
                 (result < 0 ? ffmpegError(result)
                             : "source audio layout/rate was not retained");
    }
    avcodec_free_context(&context);
    return false;
  }
  stream->encoder = context;
  stream->encoderName = codec->name;
  return true;
}

bool ExportPipeline::openEncoder(EncodedStream* stream, std::string* error) {
  const AVCodecID codecId = stream->inputStream->codecpar->codec_id;
  if (avformat_query_codec(output_->oformat, codecId,
                           FF_COMPLIANCE_NORMAL) == 0) {
    setError(error, "The " + std::string(output_->oformat->name) +
                        " muxer cannot retain " + avcodec_get_name(codecId) +
                        " for the " +
                        av_get_media_type_string(stream->type) + " stream.");
    return false;
  }
  std::vector<const AVCodec*> candidates = encoderCandidates(codecId);
  const AVFieldOrder fieldOrder = stream->inputStream->codecpar->field_order;
  if (stream->video() && fieldOrder != AV_FIELD_UNKNOWN &&
      fieldOrder != AV_FIELD_PROGRESSIVE) {
    // Field support varies by GPU generation and some hardware backends open
    // successfully while emitting progressive/unspecified bitstreams. Prefer
    // a software candidate whose field mode is driven by the AVCodec flags;
    // final probing below remains the authority.
    std::stable_partition(candidates.begin(), candidates.end(),
                          [](const AVCodec* codec) {
                            return codec &&
                                   !(codec->capabilities &
                                     AV_CODEC_CAP_HARDWARE);
                          });
  }
  std::string lastFailure;
  for (const AVCodec* codec : candidates) {
    const bool opened = stream->video()
                            ? tryVideoEncoder(stream, codec, &lastFailure)
                            : tryAudioEncoder(stream, codec, &lastFailure);
    if (opened) return true;
  }
  setError(error,
           "No installed encoder can preserve the source " +
               std::string(av_get_media_type_string(stream->type)) +
               " codec " + avcodec_get_name(codecId) +
               (lastFailure.empty() ? "." : " (" + lastFailure + ")."));
  return false;
}

bool copyCodedSideData(const AVCodecParameters* source,
                       AVCodecParameters* destination,
                       AVPacketSideDataType type) {
  const AVPacketSideData* sideData = av_packet_side_data_get(
      source->coded_side_data, source->nb_coded_side_data, type);
  if (!sideData || !sideData->data || sideData->size == 0) return true;
  av_packet_side_data_remove(destination->coded_side_data,
                             &destination->nb_coded_side_data, type);
  AVPacketSideData* copy = av_packet_side_data_new(
      &destination->coded_side_data, &destination->nb_coded_side_data, type,
      sideData->size, 0);
  if (!copy) return false;
  std::memcpy(copy->data, sideData->data, sideData->size);
  return true;
}

void ExportPipeline::captureStreamStaticMetadata() {
  const AVCodecParameters* source =
      input_->streams[selectedVideoIndex_]->codecpar;
  const struct Mapping {
    AVPacketSideDataType packetType;
    AVFrameSideDataType frameType;
  } mappings[] = {
      {AV_PKT_DATA_MASTERING_DISPLAY_METADATA,
       AV_FRAME_DATA_MASTERING_DISPLAY_METADATA},
      {AV_PKT_DATA_CONTENT_LIGHT_LEVEL, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL},
      {AV_PKT_DATA_ICC_PROFILE, AV_FRAME_DATA_ICC_PROFILE},
      {AV_PKT_DATA_AMBIENT_VIEWING_ENVIRONMENT,
       AV_FRAME_DATA_AMBIENT_VIEWING_ENVIRONMENT},
  };
  for (const Mapping& mapping : mappings) {
    const AVPacketSideData* sideData = av_packet_side_data_get(
        source->coded_side_data, source->nb_coded_side_data,
        mapping.packetType);
    if (!sideData || !sideData->data || sideData->size == 0) continue;
    staticMetadata_.capture(mapping.frameType, sideData->data, sideData->size);
  }
  if (selectedVideoIndex_ >= 0 &&
      selectedVideoIndex_ < static_cast<int>(encodedByInput_.size())) {
    const EncodedStream* video = encodedByInput_[selectedVideoIndex_];
    if (video) staticMetadata_.capture(video->decoder);
  }
}

bool ExportPipeline::probeStaticVideoMetadata(std::string* error) {
  if (selectedVideoIndex_ < 0 ||
      selectedVideoIndex_ >= static_cast<int>(encodedByInput_.size()) ||
      !encodedByInput_[selectedVideoIndex_] ||
      request_.decisions.keptRanges.empty()) {
    setError(error, "Could not inspect the retained video metadata.");
    return false;
  }
  EncodedStream* video = encodedByInput_[selectedVideoIndex_];
  AVFormatContext* format = avformat_alloc_context();
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  bool foundFrame = false;
  bool passedRange = false;
  if (!format) {
    setError(error, "Could not allocate the HDR metadata probe.");
    return false;
  }
  format->interrupt_callback.callback = &ExportPipeline::interrupt;
  format->interrupt_callback.opaque = this;
  const std::string sourceUtf8 = toUtf8String(request_.sourcePath);
  int result = avformat_open_input(&format, sourceUtf8.c_str(), nullptr, nullptr);
  if (result >= 0) result = avformat_find_stream_info(format, nullptr);
  if (result < 0 || !format) {
    setError(error, "Could not open the source HDR metadata probe: " +
                        ffmpegError(result));
    goto cleanup;
  }
  if (selectedVideoIndex_ >= static_cast<int>(format->nb_streams) ||
      format->streams[selectedVideoIndex_]->codecpar->codec_id !=
          video->decoder->codec_id) {
    setError(error,
             "The selected video stream changed during metadata preflight.");
    result = AVERROR_INVALIDDATA;
    goto cleanup;
  }
  {
    const SourceRange& firstRange = request_.decisions.keptRanges.front();
    const int64_t probeStartUs =
        format->start_time != AV_NOPTS_VALUE ? format->start_time : 0;
    if (firstRange.startUs > 0) {
      const int64_t targetTimestamp = av_rescale_q(
          probeStartUs + firstRange.startUs, kMicrosecondTimeBase,
          format->streams[selectedVideoIndex_]->time_base);
      result = avformat_seek_file(format, selectedVideoIndex_,
                                  std::numeric_limits<int64_t>::min(),
                                  targetTimestamp, targetTimestamp,
                                  AVSEEK_FLAG_BACKWARD);
      if (result < 0) {
        result = av_seek_frame(format, selectedVideoIndex_, targetTimestamp,
                               AVSEEK_FLAG_BACKWARD);
      }
      if (result < 0) {
        setError(error,
                 "Could not seek to the first retained frame for HDR "
                 "metadata inspection: " +
                     ffmpegError(result));
        goto cleanup;
      }
    }

    avcodec_flush_buffers(video->decoder);
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame) {
      setError(error, "Could not allocate the HDR metadata probe buffers.");
      goto cleanup;
    }

    const auto receiveFrames = [&]() -> bool {
      for (;;) {
        av_frame_unref(frame);
        const int receiveResult =
            avcodec_receive_frame(video->decoder, frame);
        if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
          return true;
        }
        if (receiveResult < 0) {
          result = receiveResult;
          return false;
        }
        int64_t timestamp = frame->best_effort_timestamp;
        if (timestamp == AV_NOPTS_VALUE) timestamp = frame->pts;
        if (timestamp == AV_NOPTS_VALUE) continue;
        const int64_t frameUs =
            av_rescale_q(timestamp,
                         format->streams[selectedVideoIndex_]->time_base,
                         kMicrosecondTimeBase) -
            probeStartUs;
        if (frameUs < firstRange.startUs) continue;
        if (frameUs >= firstRange.endUs) {
          passedRange = true;
          return true;
        }
        staticMetadata_.capture(frame);
        staticMetadata_.capture(video->decoder);
        foundFrame = true;
        return true;
      }
    };

    while (!foundFrame && !passedRange && !cancelled() &&
           (result = av_read_frame(format, packet)) >= 0) {
      if (packet->stream_index != selectedVideoIndex_) {
        av_packet_unref(packet);
        continue;
      }
      result = avcodec_send_packet(video->decoder, packet);
      if (result == AVERROR(EAGAIN)) {
        if (!receiveFrames()) goto cleanup;
        result = avcodec_send_packet(video->decoder, packet);
      }
      av_packet_unref(packet);
      if (result < 0 || !receiveFrames()) goto cleanup;
    }
    if (!foundFrame && !cancelled() && result == AVERROR_EOF) {
      result = avcodec_send_packet(video->decoder, nullptr);
      if (result >= 0 || result == AVERROR_EOF) {
        if (!receiveFrames()) goto cleanup;
      }
    }
  }

  if (cancelled()) {
    result = AVERROR_EXIT;
  } else if (!foundFrame) {
    setError(error,
             "The first retained range contains no decodable video frame.");
    result = AVERROR_INVALIDDATA;
  } else {
    result = 0;
  }

cleanup:
  avcodec_flush_buffers(video->decoder);
  av_frame_free(&frame);
  av_packet_free(&packet);
  avformat_close_input(&format);
  if (result < 0 && error && error->empty() && !cancelled()) {
    setError(error, "Could not inspect the retained HDR metadata: " +
                        ffmpegError(result));
  }
  return result >= 0;
}

bool ExportPipeline::createEncodedOutputStream(EncodedStream* stream,
                                               std::string* error) {
  if (!openEncoder(stream, error)) return false;
  stream->outputStream = avformat_new_stream(output_, nullptr);
  if (!stream->outputStream) {
    setError(error, "Could not create an encoded output stream.");
    return false;
  }
  AVStream* outputStream = stream->outputStream;
  outputStream->time_base = stream->encoder->time_base;
  outputStream->id = stream->inputStream->id;
  outputStream->disposition = stream->inputStream->disposition;
  av_dict_copy(&outputStream->metadata, stream->inputStream->metadata, 0);
  int result =
      avcodec_parameters_from_context(outputStream->codecpar, stream->encoder);
  if (result < 0) {
    setError(error, "Could not publish encoded stream parameters: " +
                        ffmpegError(result));
    return false;
  }
  outputStream->codecpar->codec_tag = 0;
  if (stream->video()) {
    constexpr std::array<AVPacketSideDataType, 8> preserved = {
        AV_PKT_DATA_DISPLAYMATRIX,
        AV_PKT_DATA_STEREO3D,
        AV_PKT_DATA_SPHERICAL,
        AV_PKT_DATA_MASTERING_DISPLAY_METADATA,
        AV_PKT_DATA_CONTENT_LIGHT_LEVEL,
        AV_PKT_DATA_ICC_PROFILE,
        AV_PKT_DATA_AFD,
        AV_PKT_DATA_AMBIENT_VIEWING_ENVIRONMENT,
    };
    for (AVPacketSideDataType type : preserved) {
      if (!copyCodedSideData(stream->inputStream->codecpar,
                             outputStream->codecpar, type)) {
        setError(error, "Could not preserve source video metadata.");
        return false;
      }
    }
    videoEncoderName_ = stream->encoderName;
  }
  return true;
}

bool ExportPipeline::createCopiedOutputStream(CopiedStream* stream,
                                              std::string* error) {
  if (!stream || !stream->inputStream) return false;
  const AVCodecID codecId = stream->inputStream->codecpar->codec_id;
  if (avformat_query_codec(output_->oformat, codecId,
                           FF_COMPLIANCE_NORMAL) == 0) {
    setError(error, "The source container muxer cannot retain the " +
                        std::string(av_get_media_type_string(stream->type)) +
                        " codec " + avcodec_get_name(codecId) + ".");
    return false;
  }
  stream->outputStream = avformat_new_stream(output_, nullptr);
  if (!stream->outputStream) {
    setError(error, "Could not create a preserved output stream.");
    return false;
  }
  const int result = avcodec_parameters_copy(
      stream->outputStream->codecpar, stream->inputStream->codecpar);
  if (result < 0) {
    setError(error, "Could not copy preserved stream parameters: " +
                        ffmpegError(result));
    return false;
  }
  stream->outputStream->codecpar->codec_tag = 0;
  stream->outputStream->time_base = stream->inputStream->time_base;
  stream->outputStream->id = stream->inputStream->id;
  stream->outputStream->disposition = stream->inputStream->disposition;
  av_dict_copy(&stream->outputStream->metadata,
               stream->inputStream->metadata, 0);
  return true;
}

bool ExportPipeline::createOutputStreams(std::string* error) {
  for (unsigned index = 0; index < input_->nb_streams; ++index) {
    if (EncodedStream* encoded = encodedByInput_[index]) {
      if (!createEncodedOutputStream(encoded, error)) return false;
    } else if (const CopiedStream* copied = copiedByInput_[index]) {
      const size_t copiedIndex = static_cast<size_t>(copied - copied_.data());
      if (!createCopiedOutputStream(&copied_[copiedIndex], error)) return false;
    }
  }
  return true;
}

bool ExportPipeline::createVideoRenderPlan(std::string* error) {
  if (selectedVideoIndex_ < 0 ||
      selectedVideoIndex_ >= static_cast<int>(encodedByInput_.size())) {
    setError(error, "The selected video stream is no longer available.");
    return false;
  }
  EncodedStream* video = encodedByInput_[selectedVideoIndex_];
  if (!video || !video->decoder) {
    setError(error, "The selected video decoder is not ready.");
    return false;
  }
  const bool hasSmoothCut = std::any_of(
      request_.decisions.cutTransitions.begin(),
      request_.decisions.cutTransitions.end(),
      [](const CutTransition& transition) {
        return transition.kind == CutTransitionKind::MotionSmooth;
      });
  AVFieldOrder fieldOrder = video->inputStream->codecpar->field_order;
  if (fieldOrder == AV_FIELD_UNKNOWN) {
    fieldOrder = video->decoder->field_order;
  }
  if (hasSmoothCut && fieldOrder != AV_FIELD_UNKNOWN &&
      fieldOrder != AV_FIELD_PROGRESSIVE) {
    setError(error,
             "Smooth cut cannot yet preserve interlaced field cadence; "
             "use a hard cut for this source.");
    return false;
  }
  AVRational sourceFrameRate =
      av_guess_frame_rate(input_, video->inputStream, nullptr);
  if (sourceFrameRate.num <= 0 || sourceFrameRate.den <= 0) {
    sourceFrameRate = video->inputStream->codecpar->framerate;
  }
  if (!playback_video_composition::buildRenderPlan(
          request_.decisions.keptRanges,
          request_.decisions.cutTransitions,
          playback_video_composition::SourceTiming{
              sourceFrameRate, video->inputStream->time_base},
          &videoRenderPlan_, error)) {
    return false;
  }
  return motionTimingAudit_.reset(videoRenderPlan_, error);
}

int64_t ExportPipeline::presentationStartForRange(size_t rangeIndex) const {
  int64_t start = 0;
  for (size_t index = 0; index < rangeIndex; ++index) {
    start += request_.decisions.keptRanges[index].durationUs();
  }
  return start;
}

bool ExportPipeline::copyChapters(std::string* error) {
  int nextId = 0;
  for (unsigned chapterIndex = 0; chapterIndex < input_->nb_chapters;
       ++chapterIndex) {
    const AVChapter* chapter = input_->chapters[chapterIndex];
    const int64_t chapterStartUs =
        av_rescale_q(chapter->start, chapter->time_base, kMicrosecondTimeBase) -
        formatStartUs_;
    const int64_t chapterEndUs =
        av_rescale_q(chapter->end, chapter->time_base, kMicrosecondTimeBase) -
        formatStartUs_;
    for (size_t rangeIndex = 0;
         rangeIndex < request_.decisions.keptRanges.size();
         ++rangeIndex) {
      const SourceRange& range = request_.decisions.keptRanges[rangeIndex];
      const int64_t overlapStart = std::max(chapterStartUs, range.startUs);
      const int64_t overlapEnd = std::min(chapterEndUs, range.endUs);
      if (overlapEnd <= overlapStart) continue;
      AVChapter* copy = static_cast<AVChapter*>(av_mallocz(sizeof(AVChapter)));
      if (!copy) {
        setError(error, "Could not preserve source chapters.");
        return false;
      }
      copy->id = nextId++;
      copy->time_base = kMicrosecondTimeBase;
      const int64_t presentationStart =
          presentationStartForRange(rangeIndex);
      copy->start = presentationStart + overlapStart - range.startUs;
      copy->end = presentationStart + overlapEnd - range.startUs;
      av_dict_copy(&copy->metadata, chapter->metadata, 0);
      AVChapter** chapters = static_cast<AVChapter**>(av_realloc_array(
          output_->chapters, output_->nb_chapters + 1, sizeof(*chapters)));
      if (!chapters) {
        av_dict_free(&copy->metadata);
        av_free(copy);
        setError(error, "Could not allocate preserved source chapters.");
        return false;
      }
      output_->chapters = chapters;
      output_->chapters[output_->nb_chapters++] = copy;
    }
  }
  return true;
}

bool ExportPipeline::openOutputFile(std::string* error) {
  const std::string temporaryUtf8 = toUtf8String(temporaryPath_);
  av_freep(&output_->url);
  output_->url = av_strdup(temporaryUtf8.c_str());
  if (!output_->url) {
    setError(error, "Could not bind the temporary export path.");
    return false;
  }
  int result = 0;
  if (!(output_->oformat->flags & AVFMT_NOFILE)) {
    result = avio_open(&output_->pb, temporaryUtf8.c_str(), AVIO_FLAG_WRITE);
    if (result < 0) {
      setError(error, "Could not create the temporary export: " +
                          ffmpegError(result));
      return false;
    }
  }
  AVDictionary* options = nullptr;
  if (hasNamePart(output_->oformat->name, "mp4") ||
      hasNamePart(output_->oformat->name, "mov")) {
    av_dict_set(&options, "movflags", "+faststart", 0);
  }
  result = avformat_write_header(output_, &options);
  av_dict_free(&options);
  if (result < 0) {
    setError(error, "Could not write the preserved container header: " +
                        ffmpegError(result));
    return false;
  }
  return true;
}

AVFilterInOut* makeEndpoint(const char* label, AVFilterContext* context) {
  AVFilterInOut* endpoint = avfilter_inout_alloc();
  if (!endpoint) return nullptr;
  endpoint->name = av_strdup(label);
  if (!endpoint->name) {
    avfilter_inout_free(&endpoint);
    return nullptr;
  }
  endpoint->filter_ctx = context;
  endpoint->pad_idx = 0;
  endpoint->next = nullptr;
  return endpoint;
}

std::string ExportPipeline::filterDescription(const EncodedStream& stream,
                                              std::string* error) const {
  if (stream.video()) {
    return playback_video_composition::buildProgramFilterDescription(
        videoRenderPlan_, stream.encoder->pix_fmt, error);
  }
  const std::string prefix = "a";
  std::string description =
      "[src]asplit=" +
      std::to_string(request_.decisions.keptRanges.size());
  for (size_t index = 0; index < request_.decisions.keptRanges.size();
       ++index) {
    description += "[" + prefix + std::to_string(index) + "]";
  }
  description += ";";
  for (size_t index = 0; index < request_.decisions.keptRanges.size();
       ++index) {
    const SourceRange& range = request_.decisions.keptRanges[index];
    const std::string number = std::to_string(index);
    description += "[" + prefix + number + "]";
    const int inputRate = stream.decoder->sample_rate;
    const int outputRate = stream.encoder->sample_rate;
    const int64_t start = av_rescale_q(
        range.startUs, kMicrosecondTimeBase, AVRational{1, inputRate});
    const int64_t end = av_rescale_q(
        range.endUs, kMicrosecondTimeBase, AVRational{1, inputRate});
    const int64_t first = av_rescale_q(
        range.startUs, kMicrosecondTimeBase, AVRational{1, outputRate});
    description += "atrim=start_pts=" + std::to_string(start) +
                   ":end_pts=" + std::to_string(end) + ",aresample=" +
                   std::to_string(outputRate) + ":async=1:first_pts=" +
                   std::to_string(first) + ",asetpts=PTS-" +
                   std::to_string(first) + "[t" + number + "];";
  }
  for (size_t index = 0; index < request_.decisions.keptRanges.size();
       ++index) {
    description += "[t" + std::to_string(index) + "]";
  }
  description +=
      "concat=n=" + std::to_string(request_.decisions.keptRanges.size()) +
                 ":v=0:a=1[cat];[cat]";
  const char* sampleFormat =
      av_get_sample_fmt_name(stream.encoder->sample_fmt);
  char layout[128]{};
  av_channel_layout_describe(&stream.encoder->ch_layout, layout,
                             sizeof(layout));
  if (!sampleFormat || layout[0] == '\0') {
    setError(error, "The preserving audio format cannot be described.");
    return {};
  }
  const int64_t samples = av_rescale_q(
      editedDurationUs(request_.decisions.keptRanges), kMicrosecondTimeBase,
      AVRational{1, stream.encoder->sample_rate});
  description += "apad=whole_len=" + std::to_string(samples) +
                 ",atrim=end_sample=" + std::to_string(samples) +
                 ",aformat=sample_fmts=" + sampleFormat +
                 ":sample_rates=" +
                 std::to_string(stream.encoder->sample_rate) +
                 ":channel_layouts=" + layout;
  if (stream.encoder->frame_size > 0) {
    description += ",asetnsamples=n=" +
                   std::to_string(stream.encoder->frame_size) + ":p=1";
  }
  description += "[out]";
  return description;
}

bool ExportPipeline::createFilterGraph(EncodedStream* stream,
                                       std::string* error) {
  stream->graph = avfilter_graph_alloc();
  if (!stream->graph) {
    setError(error, "Could not allocate an edit filter graph.");
    return false;
  }
  const AVFilter* sourceFilter =
      avfilter_get_by_name(stream->video() ? "buffer" : "abuffer");
  const AVFilter* sinkFilter =
      avfilter_get_by_name(stream->video() ? "buffersink" : "abuffersink");
  if (!sourceFilter || !sinkFilter) {
    setError(error, "Required FFmpeg edit filters are unavailable.");
    return false;
  }
  char arguments[512]{};
  if (stream->video()) {
    AVRational aspect = stream->inputStream->sample_aspect_ratio;
    if (aspect.num <= 0 || aspect.den <= 0) aspect = AVRational{1, 1};
    std::snprintf(arguments, sizeof(arguments),
                  "video_size=%dx%d:pix_fmt=%d:time_base=1/%d:"
                  "pixel_aspect=%d/%d:colorspace=%d:range=%d",
                  stream->decoder->width, stream->decoder->height,
                  stream->decoder->pix_fmt, AV_TIME_BASE, aspect.num,
                  aspect.den, stream->decoder->colorspace,
                  stream->decoder->color_range);
  } else {
    const char* sampleFormat =
        av_get_sample_fmt_name(stream->decoder->sample_fmt);
    AVChannelLayout normalized{};
    if (!sampleFormat ||
        !normalizedChannelLayout(stream->decoder->ch_layout, &normalized)) {
      setError(error, "Could not describe the source audio format.");
      return false;
    }
    char layout[128]{};
    av_channel_layout_describe(&normalized, layout, sizeof(layout));
    av_channel_layout_uninit(&normalized);
    std::snprintf(arguments, sizeof(arguments),
                  "time_base=1/%d:sample_rate=%d:sample_fmt=%s:"
                  "channel_layout=%s",
                  stream->decoder->sample_rate, stream->decoder->sample_rate,
                  sampleFormat, layout);
  }
  int result = avfilter_graph_create_filter(
      &stream->source, sourceFilter, "radioify_source", arguments, nullptr,
      stream->graph);
  if (result >= 0) {
    result = avfilter_graph_create_filter(&stream->sink, sinkFilter,
                                          "radioify_sink", nullptr, nullptr,
                                          stream->graph);
  }
  if (result < 0) {
    setError(error, "Could not configure an edit filter graph: " +
                        ffmpegError(result));
    return false;
  }
  AVFilterInOut* inputs = makeEndpoint("out", stream->sink);
  AVFilterInOut* outputs = makeEndpoint("src", stream->source);
  if (!inputs || !outputs) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    setError(error, "Could not allocate edit filter endpoints.");
    return false;
  }
  const std::string description = filterDescription(*stream, error);
  if (description.empty()) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return false;
  }
  result = avfilter_graph_parse_ptr(stream->graph, description.c_str(),
                                    &inputs, &outputs, nullptr);
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  if (result >= 0) result = avfilter_graph_config(stream->graph, nullptr);
  if (result < 0) {
    setError(error, "Could not build the edit decision graph: " +
                        ffmpegError(result));
    return false;
  }
  return true;
}

bool ExportPipeline::allocateWorkBuffers(std::string* error) {
  packet_ = av_packet_alloc();
  decoded_ = av_frame_alloc();
  filtered_ = av_frame_alloc();
  if (!packet_ || !decoded_ || !filtered_) {
    setError(error, "Could not allocate export work buffers.");
    return false;
  }
  return true;
}

bool ExportPipeline::prepare(std::string* error) {
  temporaryPath_ = temporaryOutputPath(request_.destinationPath);
  std::error_code filesystemError;
  std::filesystem::remove(temporaryPath_, filesystemError);
  if (!openInput(error) || !buildStreamPlan(error)) {
    return false;
  }
  captureStreamStaticMetadata();
  if (!probeStaticVideoMetadata(error) || !createVideoRenderPlan(error) ||
      !allocateOutput(error) || !createOutputStreams(error) ||
      !copyChapters(error)) {
    return false;
  }
  for (const auto& stream : encoded_) {
    if (!createFilterGraph(stream.get(), error)) return false;
  }
  return allocateWorkBuffers(error) && openOutputFile(error);
}

int64_t ExportPipeline::relativeFramePtsUs(const EncodedStream& stream,
                                           const AVFrame* frame) const {
  int64_t timestamp = frame->best_effort_timestamp;
  if (timestamp == AV_NOPTS_VALUE) timestamp = frame->pts;
  if (timestamp != AV_NOPTS_VALUE) {
    const int64_t absoluteUs = av_rescale_q(
        timestamp, stream.inputStream->time_base, kMicrosecondTimeBase);
    return std::max<int64_t>(0, absoluteUs - formatStartUs_);
  }
  if (stream.audio() && stream.nextAudioPtsUs != AV_NOPTS_VALUE) {
    return stream.nextAudioPtsUs;
  }
  if (stream.inputStream->start_time != AV_NOPTS_VALUE) {
    return std::max<int64_t>(
        0, av_rescale_q(stream.inputStream->start_time,
                        stream.inputStream->time_base, kMicrosecondTimeBase) -
               formatStartUs_);
  }
  return 0;
}

const SourceRange* rangeContainingFrame(
    int64_t ptsUs, const std::vector<SourceRange>& ranges) {
  for (const SourceRange& range : ranges) {
    if (ptsUs < range.startUs) return nullptr;
    if (ptsUs < range.endUs) return &range;
  }
  return nullptr;
}

bool ExportPipeline::submitFrame(
    EncodedStream* stream, AVFrame* frame,
    const std::function<void(int64_t)>& progress, std::string* error) {
  if (stream->sourceSatisfied) return true;
  const int64_t ptsUs = relativeFramePtsUs(*stream, frame);
  if (stream->video()) {
    const int64_t sourceFrameDurationUs =
        frame->duration > 0
            ? av_rescale_q(frame->duration, stream->inputStream->time_base,
                           kMicrosecondTimeBase)
            : 0;
    if (stream->inputIndex == selectedVideoIndex_ &&
        !motionTimingAudit_.observe(ptsUs, sourceFrameDurationUs, error)) {
      return false;
    }
    staticMetadata_.capture(frame);
    if (const SourceRange* retainedRange = rangeContainingFrame(
            ptsUs, request_.decisions.keptRanges)) {
      sourceMetadata_.observe(frame);
      const int64_t availableUs = retainedRange->endUs - ptsUs;
      stream->finalVideoFrameDurationUs = std::max<int64_t>(
          1, sourceFrameDurationUs > 0
                 ? std::min(sourceFrameDurationUs, availableUs)
                 : availableUs);
    }
    frame->pts = ptsUs;
    frame->time_base = kMicrosecondTimeBase;
    if (frame->duration > 0) {
      frame->duration = av_rescale_q(frame->duration,
                                     stream->inputStream->time_base,
                                     kMicrosecondTimeBase);
    }
  } else {
    const int sampleRate = std::max(1, stream->decoder->sample_rate);
    frame->pts = av_rescale_q(ptsUs, kMicrosecondTimeBase,
                              AVRational{1, sampleRate});
    frame->time_base = AVRational{1, sampleRate};
    frame->duration = frame->nb_samples;
    stream->nextAudioPtsUs =
        ptsUs + av_rescale_q(frame->nb_samples, AVRational{1, sampleRate},
                             kMicrosecondTimeBase);
  }
  const int result = av_buffersrc_add_frame_flags(
      stream->source, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
  if (result == AVERROR_EOF) {
    stream->sourceSatisfied = true;
    return true;
  }
  if (result < 0) {
    setError(error, "Could not feed the " +
                        std::string(stream->video() ? "video" : "audio") +
                        " edit graph: " + ffmpegError(result));
    return false;
  }
  if (stream->video() && progress) progress(ptsUs);
  return drainAvailable(stream, error);
}

bool ExportPipeline::receiveFrames(
    EncodedStream* stream, const std::function<void(int64_t)>& progress,
    std::string* error) {
  for (;;) {
    av_frame_unref(decoded_);
    const int result = avcodec_receive_frame(stream->decoder, decoded_);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
    if (result < 0) {
      setError(error, "Decoding the source " +
                          std::string(av_get_media_type_string(stream->type)) +
                          " stream failed: " + ffmpegError(result));
      return false;
    }
    if (!submitFrame(stream, decoded_, progress, error)) return false;
    if (cancelled()) return false;
  }
}

bool ExportPipeline::submitPacket(
    EncodedStream* stream, AVPacket* packet,
    const std::function<void(int64_t)>& progress, std::string* error) {
  int result = avcodec_send_packet(stream->decoder, packet);
  if (result == AVERROR(EAGAIN)) {
    if (!receiveFrames(stream, progress, error)) return false;
    result = avcodec_send_packet(stream->decoder, packet);
  }
  if (result < 0 && result != AVERROR_EOF) {
    setError(error, "Could not submit source " +
                        std::string(av_get_media_type_string(stream->type)) +
                        " packets to the decoder: " + ffmpegError(result));
    return false;
  }
  return receiveFrames(stream, progress, error);
}

bool ExportPipeline::writeEncoderPackets(EncodedStream* stream,
                                         std::string* error) {
  AVPacket* encoded = av_packet_alloc();
  if (!encoded) {
    setError(error, "Could not allocate an encoded packet.");
    return false;
  }
  bool ok = true;
  for (;;) {
    const int result = avcodec_receive_packet(stream->encoder, encoded);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
    if (result < 0) {
      setError(error, "Encoding failed in " + stream->encoderName + ": " +
                          ffmpegError(result));
      ok = false;
      break;
    }
    av_packet_rescale_ts(encoded, stream->encoder->time_base,
                         stream->outputStream->time_base);
    encoded->stream_index = stream->outputStream->index;
    const int writeResult = av_interleaved_write_frame(output_, encoded);
    av_packet_unref(encoded);
    if (writeResult < 0) {
      setError(error, "Could not mux encoded media: " +
                          ffmpegError(writeResult));
      ok = false;
      break;
    }
  }
  av_packet_free(&encoded);
  return ok;
}

bool ExportPipeline::encodeFrame(EncodedStream* stream, AVFrame* frame,
                                 AVRational sourceTimeBase,
                                 std::string* error) {
  if (frame) {
    if (stream->video()) {
      if (!staticMetadata_.apply(frame, error)) return false;
      expectedMetadata_.observe(frame);
      // Project the render graph's PTS directly onto the finalized container
      // clock.  This accounts for the muxer's representable precision without
      // blessing any additional quantisation introduced by the encoder.
      expectedVideoCadence_.observe(
          av_rescale_q(frame->pts, sourceTimeBase,
                       stream->outputStream->time_base),
          stream->outputStream->time_base);
      frame->pict_type = AV_PICTURE_TYPE_NONE;
    }
    frame->pts =
        av_rescale_q(frame->pts, sourceTimeBase, stream->encoder->time_base);
    if (frame->duration > 0) {
      frame->duration = av_rescale_q(frame->duration, sourceTimeBase,
                                     stream->encoder->time_base);
    }
    frame->time_base = stream->encoder->time_base;
  }
  int result = avcodec_send_frame(stream->encoder, frame);
  if (result == AVERROR(EAGAIN)) {
    if (!writeEncoderPackets(stream, error)) return false;
    result = avcodec_send_frame(stream->encoder, frame);
  }
  if (result < 0 && result != AVERROR_EOF) {
    setError(error, "Could not submit a frame to " + stream->encoderName +
                        ": " + ffmpegError(result));
    return false;
  }
  return writeEncoderPackets(stream, error);
}

bool ExportPipeline::queueFilteredFrame(EncodedStream* stream, AVFrame* frame,
                                        AVRational sourceTimeBase,
                                        std::string* error) {
  if (!stream->video()) {
    return encodeFrame(stream, frame, sourceTimeBase, error);
  }
  if (!stream->pendingVideoFrame) {
    stream->pendingVideoFrame = av_frame_alloc();
    if (!stream->pendingVideoFrame) {
      setError(error, "Could not allocate the video timing look-ahead.");
      return false;
    }
  }
  if (stream->pendingVideoFrame->buf[0]) {
    const int64_t nextPts = av_rescale_q(
        frame->pts, sourceTimeBase, stream->pendingVideoTimeBase);
    if (nextPts <= stream->pendingVideoFrame->pts) {
      setError(error,
               "The render graph produced non-monotonic video timestamps.");
      return false;
    }
    stream->pendingVideoFrame->duration =
        nextPts - stream->pendingVideoFrame->pts;
    if (!encodeFrame(stream, stream->pendingVideoFrame,
                     stream->pendingVideoTimeBase, error)) {
      return false;
    }
    av_frame_unref(stream->pendingVideoFrame);
  }
  const int result = av_frame_ref(stream->pendingVideoFrame, frame);
  if (result < 0) {
    setError(error, "Could not retain a filtered video frame: " +
                        ffmpegError(result));
    return false;
  }
  stream->pendingVideoTimeBase = sourceTimeBase;
  return true;
}

bool ExportPipeline::flushPendingVideoFrame(EncodedStream* stream,
                                            std::string* error) {
  if (!stream->video() || !stream->pendingVideoFrame ||
      !stream->pendingVideoFrame->buf[0]) {
    return true;
  }
  if (stream->finalVideoFrameDurationUs <= 0) {
    setError(error,
             "Could not determine the final retained video-frame duration.");
    return false;
  }
  stream->pendingVideoFrame->duration = std::max<int64_t>(
      1, av_rescale_q(stream->finalVideoFrameDurationUs,
                      kMicrosecondTimeBase,
                      stream->pendingVideoTimeBase));
  const bool encoded =
      encodeFrame(stream, stream->pendingVideoFrame,
                  stream->pendingVideoTimeBase, error);
  av_frame_unref(stream->pendingVideoFrame);
  return encoded;
}

bool ExportPipeline::drainAvailable(EncodedStream* stream,
                                    std::string* error) {
  for (;;) {
    av_frame_unref(filtered_);
    const int result = av_buffersink_get_frame(stream->sink, filtered_);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
    if (result < 0) {
      setError(error, "The edit filter graph failed: " +
                          ffmpegError(result));
      return false;
    }
    const AVRational timeBase = av_buffersink_get_time_base(stream->sink);
    if (!queueFilteredFrame(stream, filtered_, timeBase, error)) return false;
  }
}

bool ExportPipeline::drainToEnd(EncodedStream* stream, std::string* error) {
  for (;;) {
    av_frame_unref(filtered_);
    const int result = av_buffersink_get_frame(stream->sink, filtered_);
    if (result == AVERROR_EOF) return true;
    if (result == AVERROR(EAGAIN)) {
      const int requestResult = avfilter_graph_request_oldest(stream->graph);
      if (requestResult == AVERROR_EOF) return true;
      if (requestResult == AVERROR(EAGAIN)) continue;
      if (requestResult < 0) {
        setError(error, "Could not drain the edit filter graph: " +
                            ffmpegError(requestResult));
        return false;
      }
      continue;
    }
    if (result < 0) {
      setError(error, "The edit filter graph failed while finishing: " +
                          ffmpegError(result));
      return false;
    }
    const AVRational timeBase = av_buffersink_get_time_base(stream->sink);
    if (!queueFilteredFrame(stream, filtered_, timeBase, error)) return false;
    if (cancelled()) return false;
  }
}

bool ExportPipeline::copyPacket(const CopiedStream& stream,
                                const AVPacket* packet, std::string* error) {
  if (stream.attachedPicture) {
    AVPacket* copy = av_packet_clone(packet);
    if (!copy) {
      setError(error, "Could not preserve attached artwork.");
      return false;
    }
    av_packet_rescale_ts(copy, stream.inputStream->time_base,
                         stream.outputStream->time_base);
    copy->stream_index = stream.outputStream->index;
    const int result = av_interleaved_write_frame(output_, copy);
    av_packet_free(&copy);
    if (result < 0) {
      setError(error, "Could not mux attached artwork: " +
                          ffmpegError(result));
      return false;
    }
    return true;
  }

  const int64_t timestamp =
      packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
  if (timestamp == AV_NOPTS_VALUE) {
    setError(error, "A subtitle packet has no timestamp and cannot be "
                    "retimed without risking corruption.");
    return false;
  }
  const int64_t sourceStartUs =
      av_rescale_q(timestamp, stream.inputStream->time_base,
                   kMicrosecondTimeBase) -
      formatStartUs_;
  const int64_t sourceDurationUs =
      packet->duration > 0
          ? av_rescale_q(packet->duration, stream.inputStream->time_base,
                         kMicrosecondTimeBase)
          : 0;
  const int64_t sourceEndUs =
      sourceDurationUs > 0 ? sourceStartUs + sourceDurationUs
                           : sourceStartUs + 1;
  for (size_t rangeIndex = 0;
       rangeIndex < request_.decisions.keptRanges.size();
       ++rangeIndex) {
    const SourceRange& range = request_.decisions.keptRanges[rangeIndex];
    const int64_t overlapStart = std::max(sourceStartUs, range.startUs);
    const int64_t overlapEnd = std::min(sourceEndUs, range.endUs);
    if (overlapEnd <= overlapStart) continue;
    AVPacket* copy = av_packet_clone(packet);
    if (!copy) {
      setError(error, "Could not preserve a subtitle packet.");
      return false;
    }
    const int64_t presentationUs = presentationStartForRange(rangeIndex) +
                                   overlapStart - range.startUs;
    copy->pts = av_rescale_q(presentationUs, kMicrosecondTimeBase,
                             stream.outputStream->time_base);
    copy->dts = copy->pts;
    copy->duration =
        sourceDurationUs > 0
            ? av_rescale_q(overlapEnd - overlapStart, kMicrosecondTimeBase,
                           stream.outputStream->time_base)
            : 0;
    copy->stream_index = stream.outputStream->index;
    copy->pos = -1;
    const int result = av_interleaved_write_frame(output_, copy);
    av_packet_free(&copy);
    if (result < 0) {
      setError(error, "Could not mux a preserved subtitle packet: " +
                          ffmpegError(result));
      return false;
    }
  }
  return true;
}

bool ExportPipeline::finish(std::string* error) {
  for (const auto& stream : encoded_) {
    if (!stream->sourceSatisfied &&
        !submitPacket(stream.get(), nullptr, {}, error)) {
      return false;
    }
  }
  for (const auto& stream : encoded_) {
    const int closeResult =
        stream->sourceSatisfied
            ? 0
            : av_buffersrc_add_frame_flags(stream->source, nullptr, 0);
    if (closeResult < 0 && closeResult != AVERROR_EOF) {
      setError(error, "Could not close an edit filter source: " +
                          ffmpegError(closeResult));
      return false;
    }
    if (!drainToEnd(stream.get(), error) ||
        !flushPendingVideoFrame(stream.get(), error) ||
        !encodeFrame(stream.get(), nullptr, stream->encoder->time_base,
                     error)) {
      return false;
    }
  }
  if (!motionTimingAudit_.finish(videoRenderPlan_, error)) return false;
  if (!metadataSurvivedFilter(sourceMetadata_, expectedMetadata_, error)) {
    return false;
  }
  const int trailerResult = av_write_trailer(output_);
  if (trailerResult < 0) {
    setError(error, "Could not finalize the preserved container: " +
                        ffmpegError(trailerResult));
    return false;
  }
  if (!(output_->oformat->flags & AVFMT_NOFILE) && output_->pb) {
    const int closeResult = avio_closep(&output_->pb);
    if (closeResult < 0) {
      setError(error, "Could not close the completed export: " +
                          ffmpegError(closeResult));
      return false;
    }
  }
  return true;
}

bool ExportPipeline::transcode(
    const std::function<void(int64_t)>& progress, std::string* error) {
  while (!cancelled()) {
    av_packet_unref(packet_);
    const int result = av_read_frame(input_, packet_);
    if (result == AVERROR_EOF) break;
    if (result < 0) {
      setError(error, "Could not read the source during export: " +
                          ffmpegError(result));
      return false;
    }
    bool ok = true;
    const size_t index = static_cast<size_t>(packet_->stream_index);
    if (index < encodedByInput_.size() && encodedByInput_[index] &&
        !encodedByInput_[index]->sourceSatisfied) {
      ok = submitPacket(encodedByInput_[index], packet_, progress, error);
    } else if (index < copiedByInput_.size() && copiedByInput_[index]) {
      ok = copyPacket(*copiedByInput_[index], packet_, error);
    }
    av_packet_unref(packet_);
    if (!ok) return false;
  }
  if (cancelled()) return false;
  return finish(error);
}

bool sameDisplayAspect(AVRational left, AVRational right) {
  if (left.num <= 0 || left.den <= 0) left = AVRational{1, 1};
  if (right.num <= 0 || right.den <= 0) right = AVRational{1, 1};
  return av_cmp_q(left, right) == 0;
}

bool sameChannelLayout(const AVChannelLayout& left,
                       const AVChannelLayout& right) {
  AVChannelLayout normalizedLeft{};
  AVChannelLayout normalizedRight{};
  const bool valid = normalizedChannelLayout(left, &normalizedLeft) &&
                     normalizedChannelLayout(right, &normalizedRight);
  const bool same = valid &&
                    av_channel_layout_compare(&normalizedLeft,
                                              &normalizedRight) == 0;
  av_channel_layout_uninit(&normalizedLeft);
  av_channel_layout_uninit(&normalizedRight);
  return same;
}

bool dictionaryContains(const AVDictionary* expected,
                        const AVDictionary* actual) {
  const AVDictionaryEntry* entry = nullptr;
  while ((entry = av_dict_iterate(expected, entry))) {
    const AVDictionaryEntry* found =
        av_dict_get(actual, entry->key, nullptr, AV_DICT_MATCH_CASE);
    if (!found || !found->value ||
        std::string(found->value) != (entry->value ? entry->value : "")) {
      return false;
    }
  }
  return true;
}

bool chaptersMatch(const AVFormatContext* expected,
                   const AVFormatContext* actual) {
  if (!expected || !actual || expected->nb_chapters != actual->nb_chapters) {
    return false;
  }
  for (unsigned index = 0; index < expected->nb_chapters; ++index) {
    const AVChapter* before = expected->chapters[index];
    const AVChapter* after = actual->chapters[index];
    if (!before || !after) return false;
    const int64_t beforeStart =
        av_rescale_q(before->start, before->time_base, kMicrosecondTimeBase);
    const int64_t beforeEnd =
        av_rescale_q(before->end, before->time_base, kMicrosecondTimeBase);
    const int64_t afterStart =
        av_rescale_q(after->start, after->time_base, kMicrosecondTimeBase);
    const int64_t afterEnd =
        av_rescale_q(after->end, after->time_base, kMicrosecondTimeBase);
    const int64_t toleranceUs = std::max<int64_t>(
        1, std::abs(av_rescale_q(1, after->time_base,
                                 kMicrosecondTimeBase)));
    if (std::llabs(beforeStart - afterStart) > toleranceUs ||
        std::llabs(beforeEnd - afterEnd) > toleranceUs ||
        !dictionaryContains(before->metadata, after->metadata)) {
      return false;
    }
  }
  return true;
}

bool ExportPipeline::validate(std::string* error) {
  AVFormatContext* probe = nullptr;
  const std::string pathUtf8 = toUtf8String(temporaryPath_);
  int result = avformat_open_input(&probe, pathUtf8.c_str(), nullptr, nullptr);
  if (result >= 0) result = avformat_find_stream_info(probe, nullptr);
  if (result < 0 || !probe) {
    avformat_close_input(&probe);
    setError(error, "The completed export could not be validated: " +
                        ffmpegError(result));
    return false;
  }
  // Container muxers may add owned projection streams (notably the MOV/MP4
  // chapter text track) while writing the header. Validate the finalized plan
  // rather than recomputing it from source-program streams.
  const size_t plannedStreams =
      output_->nb_streams + muxerChapterProjectionCount_;
  if (probe->nb_streams != plannedStreams) {
    avformat_close_input(&probe);
    setError(error,
             "The output muxer did not retain every source media stream.");
    return false;
  }
  if (!chaptersMatch(output_, probe)) {
    avformat_close_input(&probe);
    setError(error,
             "The output muxer did not retain the retimed chapter table.");
    return false;
  }
  int outputVideoIndex = -1;
  AVPixelFormat sourcePixelFormat = AV_PIX_FMT_NONE;
  for (const auto& stream : encoded_) {
    const int outputIndex = stream->outputStream->index;
    if (outputIndex < 0 || outputIndex >= static_cast<int>(probe->nb_streams)) {
      avformat_close_input(&probe);
      setError(error, "An encoded stream is missing from the completed file.");
      return false;
    }
    const AVCodecParameters* source = stream->inputStream->codecpar;
    const AVCodecParameters* actual = probe->streams[outputIndex]->codecpar;
    if (source->codec_type != actual->codec_type ||
        source->codec_id != actual->codec_id ||
        (source->profile != AV_PROFILE_UNKNOWN &&
         source->profile != actual->profile)) {
      avformat_close_input(&probe);
      setError(error, "A source audio/video codec changed during export.");
      return false;
    }
    if (stream->video()) {
      outputVideoIndex = outputIndex;
      sourcePixelFormat = static_cast<AVPixelFormat>(stream->decoder->pix_fmt);
      if (sourcePixelFormat == AV_PIX_FMT_NONE) {
        sourcePixelFormat = static_cast<AVPixelFormat>(source->format);
      }
      const AVPixelFormat actualPixelFormat =
          static_cast<AVPixelFormat>(actual->format);
      if (source->width != actual->width || source->height != actual->height ||
          (actualPixelFormat != AV_PIX_FMT_NONE &&
           !samePixelGeometry(sourcePixelFormat, actualPixelFormat)) ||
          !sameDisplayAspect(source->sample_aspect_ratio,
                             actual->sample_aspect_ratio) ||
          (source->field_order != AV_FIELD_UNKNOWN &&
           source->field_order != actual->field_order) ||
          source->color_range != actual->color_range ||
          source->color_primaries != actual->color_primaries ||
          source->color_trc != actual->color_trc ||
          source->color_space != actual->color_space ||
          source->chroma_location != actual->chroma_location) {
        avformat_close_input(&probe);
        setError(error,
                 "Video geometry, bit depth, or HDR color "
                 "signalling changed; the completed file was rejected.");
        return false;
      }
    } else {
      const int expectedRate =
          source->sample_rate > 0 ? source->sample_rate
                                  : stream->decoder->sample_rate;
      if (actual->sample_rate != expectedRate) {
        avformat_close_input(&probe);
        setError(error, "An audio stream changed sample rate from " +
                            std::to_string(expectedRate) + " to " +
                            std::to_string(actual->sample_rate) +
                            "; the completed file was rejected.");
        return false;
      }
      if (!sameChannelLayout(source->ch_layout, actual->ch_layout)) {
        avformat_close_input(&probe);
        setError(error,
                 "An audio stream changed channel layout; the completed file "
                 "was rejected.");
        return false;
      }
      const AVCodecDescriptor* audioDescription =
          avcodec_descriptor_get(source->codec_id);
      const bool losslessAudio =
          audioDescription &&
          (audioDescription->props & AV_CODEC_PROP_LOSSLESS) != 0;
      if (losslessAudio && source->bits_per_raw_sample > 0 &&
          source->bits_per_raw_sample != actual->bits_per_raw_sample) {
        avformat_close_input(&probe);
        setError(error, "An audio stream changed raw bit depth from " +
                            std::to_string(source->bits_per_raw_sample) +
                            " to " +
                            std::to_string(actual->bits_per_raw_sample) +
                            "; the completed file was rejected.");
        return false;
      }
      if (losslessAudio && source->bits_per_coded_sample > 0 &&
          source->bits_per_coded_sample != actual->bits_per_coded_sample) {
        avformat_close_input(&probe);
        setError(error, "An audio stream changed coded bit depth from " +
                            std::to_string(source->bits_per_coded_sample) +
                            " to " +
                            std::to_string(actual->bits_per_coded_sample) +
                            "; the completed file was rejected.");
        return false;
      }
    }
  }
  for (const CopiedStream& stream : copied_) {
    const int outputIndex = stream.outputStream->index;
    if (outputIndex < 0 || outputIndex >= static_cast<int>(probe->nb_streams) ||
        probe->streams[outputIndex]->codecpar->codec_type != stream.type ||
        probe->streams[outputIndex]->codecpar->codec_id !=
            stream.inputStream->codecpar->codec_id) {
      avformat_close_input(&probe);
      setError(error,
               "A subtitle or attachment codec changed during export; the "
               "completed file was rejected.");
      return false;
    }
  }
  const int64_t expectedDuration =
      editedDurationUs(request_.decisions.keptRanges);
  const int64_t durationDifference =
      probe->duration >= expectedDuration ? probe->duration - expectedDuration
                                          : expectedDuration - probe->duration;
  if (probe->duration <= 0 || durationDifference > 150'000) {
    avformat_close_input(&probe);
    setError(error,
             "The completed timeline duration does not match the edit list.");
    return false;
  }
  avformat_close_input(&probe);

  DecodedVideoAudit audit;
  if (!decodeStreamAudit(temporaryPath_, outputVideoIndex, &audit, error)) {
    return false;
  }
  if (!samePixelGeometry(sourcePixelFormat, audit.pixelFormat)) {
    setError(error,
             "Decoded output bit depth/chroma does not match the source; the "
             "completed file was rejected.");
    return false;
  }
  if (!equalFrameCadence(expectedVideoCadence_, audit.cadence, error)) {
    return false;
  }
  if (!equalCriticalMetadata(expectedMetadata_, audit.metadata, error)) {
    return false;
  }
  for (const auto& stream : encoded_) {
    if (stream->audio() &&
        !decodeStreamAudit(temporaryPath_, stream->outputStream->index, nullptr,
                           error)) {
      return false;
    }
  }
  return true;
}

bool ExportPipeline::publish(std::string* error) {
  if (!MoveFileExW(temporaryPath_.c_str(), request_.destinationPath.c_str(),
                   MOVEFILE_WRITE_THROUGH)) {
    const DWORD moveError = GetLastError();
    setError(error, "Could not publish the validated export (Windows error " +
                        std::to_string(moveError) + ").");
    return false;
  }
  temporaryPath_.clear();
  return true;
}

}  // namespace

PipelineResult runExportPipeline(
    const ExportRequest& request, std::atomic<bool>* cancelled,
    const std::function<void(double)>& reportProgress) {
  PipelineResult result;
  if (request.sourcePath.empty() || request.destinationPath.empty() ||
      !validRanges(request.decisions.keptRanges) ||
      !request.decisions.hasValidShape()) {
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

  ExportPipeline pipeline(request, cancelled);
  if (!pipeline.prepare(&result.error)) {
    result.videoEncoder = pipeline.videoEncoderName();
    if (pipeline.cancelled()) {
      result.state = ExportState::Cancelled;
      result.error.clear();
    }
    return result;
  }
  result.videoEncoder = pipeline.videoEncoderName();
  const int64_t progressEndUs = request.decisions.keptRanges.back().endUs;
  const auto progress = [&](int64_t sourceUs) {
    if (!reportProgress || progressEndUs <= 0) return;
    reportProgress(std::clamp(static_cast<double>(sourceUs) /
                                  static_cast<double>(progressEndUs),
                              0.0, 0.98));
  };
  if (!pipeline.transcode(progress, &result.error) || pipeline.cancelled()) {
    result.state = pipeline.cancelled() ? ExportState::Cancelled
                                        : ExportState::Failed;
    if (result.state == ExportState::Cancelled) result.error.clear();
    return result;
  }
  if (reportProgress) reportProgress(0.99);
  if (!pipeline.validate(&result.error) || !pipeline.publish(&result.error)) {
    result.state = ExportState::Failed;
    return result;
  }
  if (reportProgress) reportProgress(1.0);
  result.state = ExportState::Succeeded;
  result.error.clear();
  return result;
}

}  // namespace playback_video_edit::detail
