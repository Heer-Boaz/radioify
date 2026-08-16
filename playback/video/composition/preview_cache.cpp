#include "playback/video/composition/preview_cache.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace playback_video_composition {
namespace {

constexpr int64_t kPrefetchDistanceUs = 1'000'000;
constexpr int kMaximumDecodeFrames = 2048;
constexpr auto kRenderDeadline = std::chrono::seconds(10);

int64_t steadyNowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

AVColorSpace avColorSpace(YuvMatrix matrix) {
  switch (matrix) {
    case YuvMatrix::Bt601:
      return AVCOL_SPC_SMPTE170M;
    case YuvMatrix::Bt2020:
      return AVCOL_SPC_BT2020_NCL;
    case YuvMatrix::Bt709:
      return AVCOL_SPC_BT709;
  }
  return AVCOL_SPC_BT709;
}

AVColorTransferCharacteristic avColorTransfer(YuvTransfer transfer) {
  switch (transfer) {
    case YuvTransfer::Pq:
      return AVCOL_TRC_SMPTE2084;
    case YuvTransfer::Hlg:
      return AVCOL_TRC_ARIB_STD_B67;
    case YuvTransfer::Sdr:
      return AVCOL_TRC_BT709;
  }
  return AVCOL_TRC_BT709;
}

AVColorPrimaries avColorPrimaries(YuvMatrix matrix) {
  return matrix == YuvMatrix::Bt2020 ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
}

AVPixelFormat avPixelFormat(VideoPixelFormat format) {
  switch (format) {
    case VideoPixelFormat::NV12:
      return AV_PIX_FMT_NV12;
    case VideoPixelFormat::P010:
      return AV_PIX_FMT_P010LE;
    default:
      return AV_PIX_FMT_NONE;
  }
}

std::string ffmpegError(int result) {
  char text[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(result, text, sizeof(text));
  return text;
}

AVFilterInOut* endpoint(const char* name, AVFilterContext* context) {
  AVFilterInOut* value = avfilter_inout_alloc();
  if (!value) return nullptr;
  value->name = av_strdup(name);
  if (!value->name) {
    avfilter_inout_free(&value);
    return nullptr;
  }
  value->filter_ctx = context;
  value->pad_idx = 0;
  value->next = nullptr;
  return value;
}

struct FilterGraph {
  AVFilterGraph* graph = nullptr;
  AVFilterContext* source = nullptr;
  AVFilterContext* sink = nullptr;

  ~FilterGraph() { avfilter_graph_free(&graph); }
};

struct FrameOwner {
  AVFrame* frame = av_frame_alloc();
  ~FrameOwner() { av_frame_free(&frame); }
};

bool allocateInputFrame(const VideoFrame& source, int64_t pts,
                        int64_t durationUs, AVFrame** destination) {
  if (!destination || source.width <= 0 || source.height <= 0 ||
      source.stride <= 0 || source.planeHeight < source.height ||
      source.yuv.empty()) {
    return false;
  }
  const AVPixelFormat format = avPixelFormat(source.format);
  if (format == AV_PIX_FMT_NONE) return false;
  const size_t bytesPerSample =
      source.format == VideoPixelFormat::P010 ? size_t{2} : size_t{1};
  const size_t rowBytes = static_cast<size_t>(source.width) * bytesPerSample;
  const size_t sourceStride = static_cast<size_t>(source.stride);
  const size_t sourcePlaneHeight = static_cast<size_t>(source.planeHeight);
  if (sourceStride < rowBytes ||
      sourcePlaneHeight >
          (std::numeric_limits<size_t>::max)() / sourceStride) {
    return false;
  }
  const size_t sourceYBytes = sourceStride * sourcePlaneHeight;
  const size_t chromaRows = static_cast<size_t>(source.height / 2);
  if (chromaRows >
          ((std::numeric_limits<size_t>::max)() - sourceYBytes) /
              sourceStride ||
      source.yuv.size() < sourceYBytes + chromaRows * sourceStride) {
    return false;
  }

  FrameOwner owned;
  if (!owned.frame) return false;
  owned.frame->format = format;
  owned.frame->width = source.width;
  owned.frame->height = source.height;
  owned.frame->pts = pts;
  owned.frame->duration = durationUs;
  owned.frame->time_base = AVRational{1, AV_TIME_BASE};
  owned.frame->sample_aspect_ratio = AVRational{1, 1};
  owned.frame->color_range =
      source.fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
  owned.frame->colorspace = avColorSpace(source.yuvMatrix);
  owned.frame->color_trc = avColorTransfer(source.yuvTransfer);
  owned.frame->color_primaries = avColorPrimaries(source.yuvMatrix);
  if (av_frame_get_buffer(owned.frame, 32) < 0) return false;

  for (int row = 0; row < source.height; ++row) {
    std::memcpy(owned.frame->data[0] + row * owned.frame->linesize[0],
                source.yuv.data() + static_cast<size_t>(row) * sourceStride,
                rowBytes);
  }
  const uint8_t* sourceChroma = source.yuv.data() + sourceYBytes;
  for (int row = 0; row < source.height / 2; ++row) {
    std::memcpy(owned.frame->data[1] + row * owned.frame->linesize[1],
                sourceChroma + static_cast<size_t>(row) * sourceStride,
                rowBytes);
  }
  *destination = owned.frame;
  owned.frame = nullptr;
  return true;
}

bool copyOutputFrame(const AVFrame* source, const VideoFrame& metadata,
                     int64_t timestampUs, int64_t durationUs,
                     VideoFrame* destination) {
  if (!source || !destination || source->width <= 0 || source->height <= 0 ||
      (source->width & 1) != 0 || (source->height & 1) != 0) {
    return false;
  }
  const AVPixelFormat format = static_cast<AVPixelFormat>(source->format);
  const bool p010 = format == AV_PIX_FMT_P010LE;
  if (!p010 && format != AV_PIX_FMT_NV12) return false;
  const size_t bytesPerSample = p010 ? size_t{2} : size_t{1};
  const size_t stride = static_cast<size_t>(source->width) * bytesPerSample;
  const size_t planeHeight = static_cast<size_t>(source->height);
  if (planeHeight > (std::numeric_limits<size_t>::max)() / stride) {
    return false;
  }
  const size_t yBytes = stride * planeHeight;
  const size_t chromaRows = planeHeight / 2u;
  if (chromaRows >
      ((std::numeric_limits<size_t>::max)() - yBytes) / stride) {
    return false;
  }
  const size_t totalBytes = yBytes + chromaRows * stride;
  VideoFrame output;
  try {
    output.yuv.resize(totalBytes);
  } catch (const std::bad_alloc&) {
    return false;
  }
  if (source->linesize[0] < static_cast<int>(stride) ||
      source->linesize[1] < static_cast<int>(stride)) {
    return false;
  }
  for (int row = 0; row < source->height; ++row) {
    std::memcpy(output.yuv.data() + static_cast<size_t>(row) * stride,
                source->data[0] + row * source->linesize[0], stride);
  }
  uint8_t* destinationChroma = output.yuv.data() + yBytes;
  for (int row = 0; row < source->height / 2; ++row) {
    std::memcpy(destinationChroma + static_cast<size_t>(row) * stride,
                source->data[1] + row * source->linesize[1], stride);
  }
  output.width = source->width;
  output.height = source->height;
  output.timestamp100ns = timestampUs * 10;
  output.duration100ns = durationUs * 10;
  output.format = p010 ? VideoPixelFormat::P010 : VideoPixelFormat::NV12;
  output.rotationQuarterTurns = metadata.rotationQuarterTurns;
  output.stride = static_cast<int>(stride);
  output.planeHeight = source->height;
  output.fullRange = metadata.fullRange;
  output.yuvMatrix = metadata.yuvMatrix;
  output.yuvTransfer = metadata.yuvTransfer;
  output.storageBytes = output.yuv.size();
  *destination = std::move(output);
  return true;
}

}  // namespace

struct PreviewCache::Impl {
  struct Work {
    uint64_t generation = 0;
    uint64_t compositionId = 0;
    size_t transitionIndex = 0;
    PreviewSource source;
    std::shared_ptr<const RenderPlan> plan;
  };

  struct Cached {
    size_t transitionIndex = 0;
    MotionTransitionWindow window;
    std::vector<VideoFrame> frames;
  };

  mutable std::mutex mutex;
  std::condition_variable workAvailable;
  std::thread worker;
  PreviewSource source;
  EventCallback eventCallback;
  std::shared_ptr<const RenderPlan> plan;
  uint64_t compositionId = 0;
  std::optional<size_t> requestedTransition;
  std::optional<Cached> cached;
  uint64_t attemptedGeneration = 0;
  bool started = false;
  std::atomic<bool> stopping{false};
  std::atomic<uint64_t> workGeneration{0};
  std::atomic<uint64_t> activeGeneration{0};
  std::atomic<int64_t> deadlineUs{0};

  static int interrupt(void* opaque) {
    auto* self = static_cast<Impl*>(opaque);
    if (!self || self->stopping.load(std::memory_order_relaxed)) return 1;
    if (self->activeGeneration.load(std::memory_order_relaxed) !=
        self->workGeneration.load(std::memory_order_relaxed)) {
      return 1;
    }
    const int64_t deadline = self->deadlineUs.load(std::memory_order_relaxed);
    return deadline > 0 && steadyNowUs() >= deadline ? 1 : 0;
  }

  bool cancelled(uint64_t generation) const {
    return stopping.load(std::memory_order_relaxed) ||
           generation != workGeneration.load(std::memory_order_relaxed) ||
           (deadlineUs.load(std::memory_order_relaxed) > 0 &&
            steadyNowUs() >= deadlineUs.load(std::memory_order_relaxed));
  }

  bool decodeTimingSequence(
      VideoDecoder& decoder, const std::vector<int64_t>& expectedPtsUs,
      size_t firstPixelIndex, size_t secondPixelIndex, int64_t toleranceUs,
      uint64_t generation, std::vector<SourceFrameTiming>* observed,
      std::vector<VideoFrame>* frames) {
    if (!observed || !frames || expectedPtsUs.empty() ||
        firstPixelIndex >= expectedPtsUs.size() ||
        secondPixelIndex >= expectedPtsUs.size() ||
        firstPixelIndex >= secondPixelIndex || toleranceUs <= 0 ||
        expectedPtsUs.front() < 0 ||
        expectedPtsUs.front() >
            (std::numeric_limits<int64_t>::max)() / 10 ||
        !decoder.seekToTimestamp100ns(expectedPtsUs.front() * 10)) {
      return false;
    }
    observed->clear();
    observed->reserve(expectedPtsUs.size());
    for (int count = 0;
         count < kMaximumDecodeFrames && !cancelled(generation); ++count) {
      VideoFrame timing;
      if (!decoder.readFrame(timing, nullptr, false)) return false;
      const int64_t ptsUs =
          (std::max)(int64_t{0}, timing.timestamp100ns / 10);
      const int64_t durationUs = timing.duration100ns / 10;
      if (ptsUs < expectedPtsUs.front() &&
          !sourceTimestampMatches(ptsUs, expectedPtsUs.front(),
                                  toleranceUs)) {
        continue;
      }
      if ((ptsUs > expectedPtsUs.back() &&
           !sourceTimestampMatches(ptsUs, expectedPtsUs.back(),
                                   toleranceUs)) ||
          observed->size() >= expectedPtsUs.size()) {
        return false;
      }
      const size_t index = observed->size();
      observed->push_back({ptsUs, durationUs});
      if (index == firstPixelIndex || index == secondPixelIndex) {
        VideoFrame decoded;
        if (!decoder.redecodeLastFrame(decoded)) return false;
        frames->push_back(std::move(decoded));
      }
      if (observed->size() == expectedPtsUs.size()) {
        return !cancelled(generation);
      }
    }
    return false;
  }

  bool configureFilter(const RenderPlan& renderPlan,
                       const MotionTransitionWindow& window,
                       const VideoFrame& input, FilterGraph* filter,
                       std::string* error) {
    if (!filter) return false;
    filter->graph = avfilter_graph_alloc();
    const AVFilter* sourceFilter = avfilter_get_by_name("buffer");
    const AVFilter* sinkFilter = avfilter_get_by_name("buffersink");
    if (!filter->graph || !sourceFilter || !sinkFilter) {
      if (error) *error = "Required FFmpeg motion filters are unavailable.";
      return false;
    }
    filter->graph->nb_threads = 0;
    const AVPixelFormat format = avPixelFormat(input.format);
    if (format == AV_PIX_FMT_NONE) {
      if (error) *error = "The transition preview pixel format is unsupported.";
      return false;
    }
    char arguments[512]{};
    std::snprintf(arguments, sizeof(arguments),
                  "video_size=%dx%d:pix_fmt=%d:time_base=1/%d:"
                  "pixel_aspect=1/1:colorspace=%d:range=%d",
                  input.width, input.height, format, AV_TIME_BASE,
                  avColorSpace(input.yuvMatrix),
                  input.fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG);
    int result = avfilter_graph_create_filter(
        &filter->source, sourceFilter, "radioify_transition_source",
        arguments, nullptr, filter->graph);
    if (result >= 0) {
      result = avfilter_graph_create_filter(
          &filter->sink, sinkFilter, "radioify_transition_sink", nullptr,
          nullptr, filter->graph);
    }
    if (result < 0) {
      if (error) {
        *error = "Could not configure the transition preview graph: " +
                 ffmpegError(result);
      }
      return false;
    }
    AVFilterInOut* inputs = endpoint("out", filter->sink);
    AVFilterInOut* outputs = endpoint("src", filter->source);
    if (!inputs || !outputs) {
      avfilter_inout_free(&inputs);
      avfilter_inout_free(&outputs);
      if (error) *error = "Could not allocate transition filter endpoints.";
      return false;
    }
    const std::string description = buildTransitionFilterDescription(
        renderPlan, window, format, error);
    if (description.empty()) {
      avfilter_inout_free(&inputs);
      avfilter_inout_free(&outputs);
      return false;
    }
    result = avfilter_graph_parse_ptr(filter->graph, description.c_str(),
                                      &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (result >= 0) result = avfilter_graph_config(filter->graph, nullptr);
    if (result < 0) {
      if (error) {
        *error = "Could not build the transition preview graph: " +
                 ffmpegError(result);
      }
      return false;
    }
    return true;
  }

  bool render(const Work& work, Cached* result, std::string* error) {
    if (!result || !work.plan ||
        work.transitionIndex >= work.plan->motionTransitions.size()) {
      if (error) *error = "The requested transition render window is absent.";
      return false;
    }
    const MotionTransitionWindow window =
        work.plan->motionTransitions[work.transitionIndex];
    activeGeneration.store(work.generation, std::memory_order_relaxed);
    deadlineUs.store(
        steadyNowUs() +
            std::chrono::duration_cast<std::chrono::microseconds>(
                kRenderDeadline)
                .count(),
        std::memory_order_relaxed);

    VideoDecoder decoder;
    std::string decoderError;
    if (!decoder.init(
            work.source.path, &decoderError, false, true, nullptr,
            work.source.videoStreamIndex, &Impl::interrupt, this,
            VideoCpuOutputPrecision::PreserveSource)) {
      deadlineUs.store(0, std::memory_order_relaxed);
      if (error) {
        *error = decoderError.empty()
                     ? "Could not open the transition preview decoder."
                     : decoderError;
      }
      return false;
    }
    std::vector<VideoFrame> context;
    context.reserve(4);
    MotionSourceTiming expected;
    std::vector<SourceFrameTiming> outgoing;
    std::vector<SourceFrameTiming> incoming;
    if (!motionSourceTiming(*work.plan, window, &expected, error) ||
        !decodeTimingSequence(
            decoder, expected.outgoingPtsUs, 0, 1,
            work.plan->timestampToleranceUs, work.generation, &outgoing,
            &context) ||
        !decodeTimingSequence(
            decoder, expected.incomingPtsUs, window.incomingFrames,
            static_cast<size_t>(window.incomingFrames) + 1,
            work.plan->timestampToleranceUs, work.generation, &incoming,
            &context) ||
        !validateMotionSourceTiming(*work.plan, window, outgoing, incoming,
                                    error) ||
        context.size() != 4 || cancelled(work.generation)) {
      deadlineUs.store(0, std::memory_order_relaxed);
      if (error && error->empty() && !cancelled(work.generation)) {
        *error = "Could not decode the four transition context frames.";
      }
      return false;
    }
    const VideoFrame& metadata = context.front();
    const bool sameGeometry = std::all_of(
        context.begin(), context.end(), [&](const VideoFrame& frame) {
          return frame.format == metadata.format &&
                 frame.width == metadata.width &&
                 frame.height == metadata.height &&
                 frame.rotationQuarterTurns == metadata.rotationQuarterTurns;
        });
    if (!sameGeometry) {
      deadlineUs.store(0, std::memory_order_relaxed);
      if (error) *error = "Transition context frames changed format or size.";
      return false;
    }

    FilterGraph filter;
    if (!configureFilter(*work.plan, window, metadata, &filter, error)) {
      deadlineUs.store(0, std::memory_order_relaxed);
      return false;
    }
    for (size_t index = 0; index < context.size(); ++index) {
      AVFrame* input = nullptr;
      if (!allocateInputFrame(context[index], static_cast<int64_t>(index),
                              work.plan->frameDurationUs, &input)) {
        deadlineUs.store(0, std::memory_order_relaxed);
        if (error) *error = "Could not allocate a transition context frame.";
        return false;
      }
      const int submit = av_buffersrc_add_frame_flags(
          filter.source, input, AV_BUFFERSRC_FLAG_KEEP_REF);
      av_frame_free(&input);
      if (submit < 0) {
        deadlineUs.store(0, std::memory_order_relaxed);
        if (error) {
          *error = "Could not feed the transition preview graph: " +
                   ffmpegError(submit);
        }
        return false;
      }
    }
    if (av_buffersrc_add_frame_flags(filter.source, nullptr, 0) < 0) {
      deadlineUs.store(0, std::memory_order_relaxed);
      if (error) *error = "Could not finish the transition preview input.";
      return false;
    }

    std::vector<VideoFrame> rendered;
    rendered.reserve(window.durationFrames);
    FrameOwner filtered;
    if (!filtered.frame) {
      deadlineUs.store(0, std::memory_order_relaxed);
      if (error) *error = "Could not allocate transition preview output.";
      return false;
    }
    for (;;) {
      if (cancelled(work.generation)) {
        deadlineUs.store(0, std::memory_order_relaxed);
        return false;
      }
      av_frame_unref(filtered.frame);
      const int receive = av_buffersink_get_frame(filter.sink, filtered.frame);
      if (receive == AVERROR_EOF) break;
      if (receive == AVERROR(EAGAIN)) {
        const int requested = avfilter_graph_request_oldest(filter.graph);
        if (requested == AVERROR_EOF) break;
        if (requested == AVERROR(EAGAIN)) continue;
        if (requested < 0) {
          deadlineUs.store(0, std::memory_order_relaxed);
          if (error) {
            *error = "Could not evaluate the transition preview graph: " +
                     ffmpegError(requested);
          }
          return false;
        }
        continue;
      }
      if (receive < 0 || rendered.size() >= window.durationFrames) {
        deadlineUs.store(0, std::memory_order_relaxed);
        if (error) {
          *error = receive < 0
                       ? "Could not receive a transition preview frame: " +
                             ffmpegError(receive)
                       : "The transition preview graph produced too many "
                         "frames.";
        }
        return false;
      }
      VideoFrame output;
      const int64_t index = static_cast<int64_t>(rendered.size());
      const int64_t frameStartUs = av_rescale_q_rnd(
          index, av_inv_q(work.plan->frameRate), AVRational{1, AV_TIME_BASE},
          static_cast<AVRounding>(AV_ROUND_NEAR_INF |
                                  AV_ROUND_PASS_MINMAX));
      const int64_t frameEndUs = av_rescale_q_rnd(
          index + 1, av_inv_q(work.plan->frameRate),
          AVRational{1, AV_TIME_BASE},
          static_cast<AVRounding>(AV_ROUND_NEAR_INF |
                                  AV_ROUND_PASS_MINMAX));
      const int64_t timestampUs = window.presentationStartUs + frameStartUs;
      if (!copyOutputFrame(filtered.frame, metadata, timestampUs,
                           frameEndUs - frameStartUs, &output)) {
        deadlineUs.store(0, std::memory_order_relaxed);
        if (error) {
          *error = "The transition preview graph changed the preserving pixel "
                   "format.";
        }
        return false;
      }
      rendered.push_back(std::move(output));
    }
    deadlineUs.store(0, std::memory_order_relaxed);
    if (rendered.size() != window.durationFrames ||
        cancelled(work.generation)) {
      if (error && !cancelled(work.generation)) {
        *error = "The transition preview graph produced " +
                 std::to_string(rendered.size()) + " of " +
                 std::to_string(window.durationFrames) + " frames.";
      }
      return false;
    }
    *result = Cached{work.transitionIndex, window,
                     std::move(rendered)};
    return true;
  }

  void run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
      Work work;
      {
        std::unique_lock<std::mutex> lock(mutex);
        workAvailable.wait(lock, [&]() {
          return stopping.load(std::memory_order_relaxed) ||
                 (started && plan && requestedTransition &&
                  attemptedGeneration !=
                      workGeneration.load(std::memory_order_relaxed));
        });
        if (stopping.load(std::memory_order_relaxed)) break;
        work.generation = workGeneration.load(std::memory_order_relaxed);
        work.compositionId = compositionId;
        work.transitionIndex = *requestedTransition;
        work.source = source;
        work.plan = plan;
      }

      Cached next;
      std::string error;
      bool ready = false;
      try {
        ready = render(work, &next, &error);
      } catch (const std::exception& exception) {
        error = std::string("Transition preview failed: ") + exception.what();
      } catch (...) {
        error = "Transition preview failed unexpectedly.";
      }
      EventCallback callback;
      {
        std::lock_guard<std::mutex> lock(mutex);
        attemptedGeneration = work.generation;
        if (ready && !stopping.load(std::memory_order_relaxed) &&
            work.generation ==
                workGeneration.load(std::memory_order_relaxed) &&
            work.compositionId == compositionId && requestedTransition &&
            *requestedTransition == work.transitionIndex) {
          cached = std::move(next);
          callback = eventCallback;
        } else if (!ready && !error.empty() &&
                   !stopping.load(std::memory_order_relaxed) &&
                   work.generation ==
                       workGeneration.load(std::memory_order_relaxed)) {
          callback = eventCallback;
        }
      }
      if (callback) {
        callback(ready ? PreviewEvent{PreviewEventType::FrameChanged, {}}
                       : PreviewEvent{PreviewEventType::RenderFailed,
                                      std::move(error)});
      }
    }
    deadlineUs.store(0, std::memory_order_relaxed);
    activeGeneration.store(0, std::memory_order_relaxed);
  }
};

PreviewCache::PreviewCache() : impl_(std::make_unique<Impl>()) {}

PreviewCache::~PreviewCache() { stop(); }

bool PreviewCache::start(const PreviewSource& source,
                         EventCallback eventCallback) {
  stop();
  if (source.path.empty() || source.videoStreamIndex < 0) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->source = source;
    impl_->eventCallback = std::move(eventCallback);
    impl_->plan.reset();
    impl_->compositionId = 0;
    impl_->requestedTransition.reset();
    impl_->cached.reset();
    impl_->attemptedGeneration = 0;
    impl_->workGeneration.store(0, std::memory_order_relaxed);
    impl_->stopping.store(false, std::memory_order_relaxed);
    impl_->started = true;
  }
  try {
    impl_->worker = std::thread([this]() { impl_->run(); });
  } catch (...) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->started = false;
    impl_->source = PreviewSource{};
    impl_->eventCallback = {};
    return false;
  }
  return true;
}

void PreviewCache::stop() {
  if (!impl_) return;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->started && !impl_->worker.joinable()) return;
    impl_->stopping.store(true, std::memory_order_relaxed);
    impl_->workGeneration.fetch_add(1, std::memory_order_relaxed);
    impl_->requestedTransition.reset();
  }
  impl_->workAvailable.notify_all();
  if (impl_->worker.joinable()) impl_->worker.join();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->started = false;
    impl_->source = PreviewSource{};
    impl_->eventCallback = {};
    impl_->plan.reset();
    impl_->compositionId = 0;
    impl_->cached.reset();
    impl_->attemptedGeneration = 0;
  }
}

void PreviewCache::setPlan(uint64_t compositionId,
                           std::shared_ptr<const RenderPlan> plan,
                           int64_t focusPresentationUs) {
  EventCallback callback;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->started) return;
    const bool removedVisibleCache = impl_->cached.has_value();
    impl_->compositionId = compositionId;
    impl_->plan = std::move(plan);
    impl_->cached.reset();
    impl_->requestedTransition =
        impl_->plan
            ? impl_->plan->motionTransitionIndexNear(focusPresentationUs,
                                                     kPrefetchDistanceUs)
            : std::nullopt;
    impl_->workGeneration.fetch_add(1, std::memory_order_relaxed);
    if (removedVisibleCache) callback = impl_->eventCallback;
  }
  impl_->workAvailable.notify_one();
  if (callback) callback({PreviewEventType::FrameChanged, {}});
}

void PreviewCache::prefetchAround(uint64_t compositionId,
                                  int64_t presentationUs) {
  bool notify = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->started || !impl_->plan ||
        compositionId != impl_->compositionId) {
      return;
    }
    const std::optional<size_t> requested =
        impl_->plan->motionTransitionIndexNear(presentationUs,
                                               kPrefetchDistanceUs);
    if (!requested) {
      if (impl_->requestedTransition) {
        impl_->requestedTransition.reset();
        impl_->workGeneration.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    }
    if (impl_->cached && impl_->cached->transitionIndex == *requested) {
      return;
    }
    if (impl_->requestedTransition &&
        *impl_->requestedTransition == *requested) {
      return;
    }
    impl_->requestedTransition = requested;
    impl_->workGeneration.fetch_add(1, std::memory_order_relaxed);
    notify = true;
  }
  if (notify) impl_->workAvailable.notify_one();
}

bool PreviewCache::copyFrame(uint64_t compositionId,
                             int64_t presentationUs,
                             int64_t presentationDurationUs,
                             VideoFrame* out) const {
  if (!out) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (compositionId != impl_->compositionId || !impl_->cached ||
      impl_->cached->frames.empty()) {
    return false;
  }
  const Impl::Cached& cached = *impl_->cached;
  const int64_t sampleUs =
      presentationUs + (std::max)(int64_t{0}, presentationDurationUs) / 2;
  const int64_t offsetUs = sampleUs - cached.window.presentationStartUs;
  if (offsetUs < 0 || offsetUs >= cached.window.durationUs) return false;
  const auto selected = std::find_if(
      cached.frames.begin(), cached.frames.end(), [&](const VideoFrame& frame) {
        const int64_t startUs = frame.timestamp100ns / 10;
        const int64_t durationUs = frame.duration100ns / 10;
        return durationUs > 0 && sampleUs >= startUs &&
               sampleUs - startUs < durationUs;
      });
  if (selected == cached.frames.end()) return false;
  *out = *selected;
  out->timestamp100ns = presentationUs * 10;
  out->duration100ns = presentationDurationUs * 10;
  return true;
}

}  // namespace playback_video_composition
