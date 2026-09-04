#include "playback/video/chapter/sampled_evidence.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>
#include <utility>

#include "playback/video/decoder.h"
#include "playback/video/frame_conversion.h"
#include "playback/video/image.h"

namespace playback_video_chapters {
namespace {

constexpr int kSampleWidth = 384;
constexpr int kSampleHeight = 216;
constexpr int kMaximumDecodeFrames = 2048;
constexpr auto kSampleDeadline = std::chrono::seconds(8);

std::int64_t steadyNowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct InterruptContext {
  const OperationControl *control = nullptr;
  std::int64_t deadlineUs = 0;

  static int callback(void *opaque) {
    const auto *self = static_cast<const InterruptContext *>(opaque);
    if (!self || !self->control)
      return 1;
    try {
      if (self->control->cancelled && self->control->cancelled())
        return 1;
      if (self->control->backgroundGpuAllowed &&
          !self->control->backgroundGpuAllowed()) {
        return 1;
      }
    } catch (...) {
      return 1;
    }
    return self->deadlineUs > 0 && steadyNowUs() >= self->deadlineUs ? 1 : 0;
  }
};

OperationStatus interruptionStatus(const OperationControl &control) {
  if (control.cancelled && control.cancelled()) {
    return OperationStatus::Cancelled;
  }
  if (control.backgroundGpuAllowed && !control.backgroundGpuAllowed()) {
    return OperationStatus::Yielded;
  }
  return OperationStatus::Failed;
}

OperationStatus initializeHardwareDecoder(const AnalysisRequest &request,
                                          const OperationControl &control,
                                          InterruptContext *interrupt,
                                          VideoDecoder *decoder,
                                          std::string *detail) {
  if (!interrupt || !decoder)
    return OperationStatus::Failed;
  std::string decoderError;
  if (!decoder->init(request.file, &decoderError, true, true, nullptr,
                     request.videoStreamIndex, &InterruptContext::callback,
                     interrupt, VideoCpuOutputPrecision::EightBit,
                     VideoHardwareDecodePolicy::RequireD3D11)) {
    const OperationStatus interrupted = interruptionStatus(control);
    if (interrupted == OperationStatus::Cancelled) {
      if (detail)
        *detail = "Chapter analysis cancelled.";
      return interrupted;
    }
    if (interrupted == OperationStatus::Yielded) {
      if (detail)
        *detail = "Playback reclaimed the GPU.";
      return interrupted;
    }
    if (detail) {
      *detail = decoderError.empty()
                    ? "D3D11 hardware video decoding is unavailable."
                    : std::move(decoderError);
    }
    return OperationStatus::Unsupported;
  }

  const int sourceWidth = std::max(
      2, decoder->width() > 0 ? decoder->width() : request.sourceWidth);
  const int sourceHeight = std::max(
      2, decoder->height() > 0 ? decoder->height() : request.sourceHeight);
  const double scale =
      std::min(static_cast<double>(kSampleWidth) / sourceWidth,
               static_cast<double>(kSampleHeight) / sourceHeight);
  int decodeWidth = std::max(2, static_cast<int>(sourceWidth * scale));
  int decodeHeight = std::max(2, static_cast<int>(sourceHeight * scale));
  decodeWidth &= ~1;
  decodeHeight &= ~1;
  if (!decoder->setTargetSize(decodeWidth, decodeHeight, &decoderError)) {
    if (detail) {
      *detail = decoderError.empty()
                    ? "The video cannot be scaled for chapter analysis."
                    : std::move(decoderError);
    }
    return OperationStatus::Unsupported;
  }
  if (detail)
    detail->clear();
  return OperationStatus::Succeeded;
}

bool sampleFrame(VideoDecoder *decoder, InterruptContext *interrupt,
                 std::int64_t targetUs,
                 playback_video_image::RgbaImage *image) {
  if (!decoder || !interrupt || !image || targetUs < 0)
    return false;
  interrupt->deadlineUs =
      steadyNowUs() +
      std::chrono::duration_cast<std::chrono::microseconds>(kSampleDeadline)
          .count();
  if (targetUs > (std::numeric_limits<std::int64_t>::max)() / 10 ||
      !decoder->seekToTimestamp100ns(targetUs * 10)) {
    return false;
  }
  bool haveCandidate = false;
  bool reachedTarget = false;
  for (int decoded = 0; decoded < kMaximumDecodeFrames; ++decoded) {
    if (InterruptContext::callback(interrupt))
      return false;
    VideoFrame frame;
    if (!decoder->readFrame(frame, nullptr, false))
      break;
    const std::int64_t ptsUs =
        std::max<std::int64_t>(0, frame.timestamp100ns / 10);
    std::int64_t durationUs = frame.duration100ns / 10;
    if (durationUs <= 0)
      durationUs = 33333;
    haveCandidate = true;
    if (ptsUs >= targetUs ||
        (ptsUs <= targetUs && durationUs > targetUs - ptsUs)) {
      reachedTarget = true;
      break;
    }
  }
  VideoFrame selected;
  return haveCandidate && (reachedTarget || decoder->reachedEndOfStream()) &&
         !InterruptContext::callback(interrupt) &&
         decoder->redecodeLastFrame(selected) &&
         playback_video_frame_conversion::toRgba(selected, image);
}

bool copySample(const playback_video_image::RgbaImage &image,
                std::int64_t sampleTimeUs, SampledFrame *sample) {
  if (!sample || sampleTimeUs < 0 || !playback_video_image::validate(image) ||
      image.width == 0 ||
      image.height == 0 ||
      image.width >
          (std::numeric_limits<std::size_t>::max)() / image.height / 3u) {
    return false;
  }
  sample->timeUs = sampleTimeUs;
  sample->width = image.width;
  sample->height = image.height;
  sample->rgb.resize(static_cast<std::size_t>(image.width) * image.height * 3u);
  for (std::uint32_t y = 0; y < image.height; ++y) {
    const std::uint8_t *source =
        image.pixels.data() + static_cast<std::size_t>(y) * image.strideBytes;
    std::uint8_t *destination =
        sample->rgb.data() + static_cast<std::size_t>(y) * image.width * 3u;
    for (std::uint32_t x = 0; x < image.width; ++x) {
      destination[x * 3u + 0u] = source[x * 4u + 0u];
      destination[x * 3u + 1u] = source[x * 4u + 1u];
      destination[x * 3u + 2u] = source[x * 4u + 2u];
    }
  }
  return true;
}

bool validCheckpoint(const AnalysisRequest &request,
                     const SampledEvidenceCheckpoint &checkpoint,
                     const std::vector<std::int64_t> &sampleTimesUs) {
  if (checkpoint.requestedTimesUs.empty())
    return checkpoint.frames.empty();
  if (checkpoint.requestedTimesUs != sampleTimesUs ||
      sampleTimesUs.size() < kMinimumAutomaticChapterCount ||
      sampleTimesUs.size() > kMaximumAutomaticChapterCount ||
      checkpoint.frames.size() > sampleTimesUs.size() ||
      !std::is_sorted(sampleTimesUs.begin(), sampleTimesUs.end()) ||
      std::adjacent_find(sampleTimesUs.begin(), sampleTimesUs.end()) !=
          sampleTimesUs.end() ||
      sampleTimesUs.front() < 0 || sampleTimesUs.back() >= request.durationUs) {
    return false;
  }
  for (std::size_t index = 0; index < checkpoint.frames.size(); ++index) {
    const SampledFrame &frame = checkpoint.frames[index];
    if (frame.timeUs != sampleTimesUs[index] || frame.width == 0 ||
        frame.height == 0 || frame.rgb.empty())
      return false;
  }
  return true;
}

} // namespace

OperationStatus probeHardwareVideoDecode(const AnalysisRequest &request,
                                         const OperationControl &control,
                                         std::string *detail) {
  if (detail)
    detail->clear();
  if (request.file.empty() || request.videoStreamIndex < 0 ||
      request.durationUs <= 0) {
    if (detail) {
      *detail = "The video cannot be sampled for chapter analysis.";
    }
    return OperationStatus::Unsupported;
  }
  if (control.backgroundGpuAllowed && !control.backgroundGpuAllowed()) {
    if (detail)
      *detail = "Playback has GPU priority.";
    return OperationStatus::Yielded;
  }
  if (control.progress) {
    control.progress(std::nullopt, "Testing D3D11 hardware video decode");
  }
  InterruptContext interrupt{&control, 0};
  VideoDecoder decoder;
  OperationStatus status =
      initializeHardwareDecoder(request, control, &interrupt, &decoder, detail);
  if (status != OperationStatus::Succeeded)
    return status;
  playback_video_image::RgbaImage sample;
  if (sampleFrame(&decoder, &interrupt, 0, &sample)) {
    return OperationStatus::Succeeded;
  }
  status = interruptionStatus(control);
  if (status == OperationStatus::Cancelled) {
    if (detail)
      *detail = "Chapter analysis cancelled.";
    return status;
  }
  if (status == OperationStatus::Yielded) {
    if (detail)
      *detail = "Playback reclaimed the GPU.";
    return status;
  }
  if (detail) {
    *detail = "This video cannot produce a D3D11 hardware-decoded sample.";
  }
  return OperationStatus::Unsupported;
}

SampledEvidenceResult
sampleVideoEvidence(const AnalysisRequest &request,
                    const OperationControl &control,
                    SampledEvidenceCheckpoint *checkpoint,
                    const std::vector<std::int64_t> &sampleTimesUs) {
  SampledEvidenceResult result;
  if (request.file.empty() || request.videoStreamIndex < 0 ||
      request.durationUs <= 0 || !checkpoint || sampleTimesUs.empty()) {
    result.detail = "The video cannot be sampled for chapter analysis.";
    return result;
  }
  if (!validCheckpoint(request, *checkpoint, sampleTimesUs)) {
    *checkpoint = {};
    result.detail = "The retained chapter evidence is invalid.";
    return result;
  }
  if (control.backgroundGpuAllowed && !control.backgroundGpuAllowed()) {
    result.status = OperationStatus::Yielded;
    result.detail = "Playback has GPU priority.";
    return result;
  }

  if (checkpoint->requestedTimesUs.empty())
    checkpoint->requestedTimesUs = sampleTimesUs;

  InterruptContext interrupt{&control, 0};
  VideoDecoder decoder;
  result.status = initializeHardwareDecoder(request, control, &interrupt,
                                            &decoder, &result.detail);
  if (result.status != OperationStatus::Succeeded)
    return result;
  try {
    checkpoint->frames.reserve(checkpoint->requestedTimesUs.size());
  } catch (const std::bad_alloc &) {
    result.detail = "Could not allocate the chapter video samples.";
    return result;
  }

  const std::size_t totalFrames = checkpoint->requestedTimesUs.size();
  std::size_t completedFrames = checkpoint->frames.size();
  for (std::size_t index = checkpoint->frames.size();
       index < checkpoint->requestedTimesUs.size(); ++index) {
    if ((control.cancelled && control.cancelled()) ||
        (control.backgroundGpuAllowed && !control.backgroundGpuAllowed())) {
      result.status = interruptionStatus(control);
      result.detail = result.status == OperationStatus::Yielded
                          ? "Playback reclaimed the GPU."
                          : "Chapter analysis cancelled.";
      return result;
    }
    const std::int64_t sampleTimeUs = checkpoint->requestedTimesUs[index];
    playback_video_image::RgbaImage image;
    SampledFrame sample;
    bool sampled = sampleFrame(&decoder, &interrupt, sampleTimeUs, &image);
    if (sampled) {
      try {
        sampled = copySample(image, sampleTimeUs, &sample);
      } catch (const std::bad_alloc &) {
        result.status = OperationStatus::Failed;
        result.detail = "Could not allocate a chapter video sample.";
        return result;
      }
    }
    if (!sampled) {
      result.status = interruptionStatus(control);
      if (result.status == OperationStatus::Yielded) {
        result.detail = "Playback reclaimed the GPU.";
      } else if (result.status == OperationStatus::Cancelled) {
        result.detail = "Chapter analysis cancelled.";
      } else {
        result.status = OperationStatus::Unsupported;
        result.detail = "A hardware-decoded chapter sample failed.";
      }
      return result;
    }
    checkpoint->frames.push_back(std::move(sample));
    ++completedFrames;
    if (control.progress) {
      control.progress(static_cast<double>(completedFrames) /
                           std::max<std::size_t>(1, totalFrames),
                       "Extracting chapter caption frames on D3D11");
    }
  }
  decoder.uninit();
  result.status = OperationStatus::Succeeded;
  return result;
}

} // namespace playback_video_chapters
