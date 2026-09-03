#include "playback/video/analysis/visual_timeline_scan.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#include "playback/video/decoder.h"

namespace playback_video_analysis {
namespace {

constexpr int kDecodeWidth = 192;
constexpr int kDecodeHeight = 108;

enum class Interruption : std::uint8_t {
  None,
  Cancelled,
  Yielded,
  Failed,
};

Interruption interruption(const VisualTimelineScanControl &control) {
  try {
    if (control.cancelled && control.cancelled()) {
      return Interruption::Cancelled;
    }
    if (control.backgroundGpuAllowed && !control.backgroundGpuAllowed()) {
      return Interruption::Yielded;
    }
    return Interruption::None;
  } catch (...) {
    return Interruption::Failed;
  }
}

struct DecoderInterruptContext {
  const VisualTimelineScanControl *control = nullptr;
};

int interruptDecoder(void *opaque) {
  const auto *context = static_cast<const DecoderInterruptContext *>(opaque);
  return !context || !context->control ||
                 interruption(*context->control) != Interruption::None
             ? 1
             : 0;
}

VisualTimelineScanResult
interruptedResult(const VisualTimelineScanControl &control) {
  switch (interruption(control)) {
  case Interruption::Cancelled:
    return {VisualTimelineScanStatus::Cancelled,
            "Video analysis cancelled.",
            0,
            {}};
  case Interruption::Yielded:
    return {VisualTimelineScanStatus::Yielded,
            "Playback reclaimed the GPU.",
            0,
            {}};
  case Interruption::Failed:
    return {VisualTimelineScanStatus::Failed,
            "Could not query video-analysis admission.",
            0,
            {}};
  case Interruption::None:
    break;
  }
  return {};
}

VisualTimelineScanResult
finishInterruption(const VisualTimelineScanControl &control,
                   VisualTimelineScanCheckpoint *checkpoint,
                   std::int64_t durationUs = 0) {
  VisualTimelineScanResult result = interruptedResult(control);
  result.durationUs = durationUs;
  // A yielded scan is a resumable scheduling event. Cancellation or an
  // admission-callback failure terminates the request and must not leave
  // retained work that a later request could accidentally reuse.
  if (checkpoint && result.status != VisualTimelineScanStatus::Yielded) {
    *checkpoint = {};
  }
  return result;
}

bool report(const VisualTimelineScanControl &control, double progress) {
  try {
    if (control.progress) {
      control.progress(std::clamp(progress, 0.0, 1.0));
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool frameLumaAt(const VideoFrame &frame, int x, int y, std::uint8_t *luma) {
  if (!luma || frame.width <= 0 || frame.height <= 0 || x < 0 || y < 0 ||
      x >= frame.width || y >= frame.height || frame.stride <= 0) {
    return false;
  }
  const std::size_t stride = static_cast<std::size_t>(frame.stride);
  if (frame.format == VideoPixelFormat::NV12) {
    const std::size_t offset =
        static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x);
    if (offset >= frame.yuv.size())
      return false;
    *luma = frame.yuv[offset];
    return true;
  }
  if (frame.format == VideoPixelFormat::P010) {
    const std::size_t offset =
        static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 2u;
    if (offset + sizeof(std::uint16_t) > frame.yuv.size())
      return false;
    std::uint16_t value = 0;
    std::memcpy(&value, frame.yuv.data() + offset, sizeof(value));
    *luma = static_cast<std::uint8_t>(std::min<std::uint16_t>(255, value >> 8));
    return true;
  }
  if (frame.format == VideoPixelFormat::RGB32 ||
      frame.format == VideoPixelFormat::ARGB32) {
    const std::size_t offset =
        static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4u;
    if (offset + 3u > frame.rgba.size())
      return false;
    const unsigned int first = frame.rgba[offset];
    const unsigned int second = frame.rgba[offset + 1u];
    const unsigned int third = frame.rgba[offset + 2u];
    // The decoder has multiple packed channel orders. A symmetric average is
    // sufficient for temporal signatures without expanding VideoFrame's
    // playback-facing colour contract.
    *luma = static_cast<std::uint8_t>((first + second + third) / 3u);
    return true;
  }
  return false;
}

bool extractVisualSample(const VideoFrame &frame, std::int64_t timestampUs,
                         VisualSample *sample) {
  if (!sample || frame.width <= 0 || frame.height <= 0)
    return false;
  VisualSample out;
  out.timestampUs = std::max<std::int64_t>(0, timestampUs);
  double lumaTotal = 0.0;
  std::size_t darkCount = 0;
  for (int gridY = 0; gridY < kFeatureGridRows; ++gridY) {
    const int y =
        std::clamp(static_cast<int>((static_cast<std::int64_t>(gridY * 2 + 1) *
                                     frame.height) /
                                    (kFeatureGridRows * 2)),
                   0, frame.height - 1);
    for (int gridX = 0; gridX < kFeatureGridColumns; ++gridX) {
      const int x = std::clamp(
          static_cast<int>(
              (static_cast<std::int64_t>(gridX * 2 + 1) * frame.width) /
              (kFeatureGridColumns * 2)),
          0, frame.width - 1);
      std::uint8_t value = 0;
      if (!frameLumaAt(frame, x, y, &value))
        return false;
      const std::size_t index =
          static_cast<std::size_t>(gridY * kFeatureGridColumns + gridX);
      out.luma[index] = value;
      lumaTotal += value;
      if (value < 24)
        ++darkCount;
    }
  }
  out.meanLuma = static_cast<float>(
      lumaTotal / static_cast<double>(kFeatureGridSize * 255));
  out.darkFraction =
      static_cast<float>(darkCount) / static_cast<float>(kFeatureGridSize);

  double bandLuma = 0.0;
  double centerLuma = 0.0;
  std::size_t bandDark = 0;
  std::size_t bandCount = 0;
  std::size_t centerCount = 0;
  double borderEdges = 0.0;
  double centerEdges = 0.0;
  std::size_t borderEdgeCount = 0;
  std::size_t centerEdgeCount = 0;
  for (int y = 0; y < kFeatureGridRows; ++y) {
    for (int x = 0; x < kFeatureGridColumns; ++x) {
      const std::size_t index =
          static_cast<std::size_t>(y * kFeatureGridColumns + x);
      const std::uint8_t value = out.luma[index];
      const bool inBand = y < 2 || y >= kFeatureGridRows - 2;
      if (inBand) {
        bandLuma += value;
        ++bandCount;
        if (value < 20)
          ++bandDark;
      } else {
        centerLuma += value;
        ++centerCount;
      }
      if (x + 1 >= kFeatureGridColumns)
        continue;
      const double edge = std::abs(static_cast<int>(value) -
                                   static_cast<int>(out.luma[index + 1u])) /
                          255.0;
      const bool border = x < 6 || x >= kFeatureGridColumns - 7 || y < 3 ||
                          y >= kFeatureGridRows - 3;
      if (border) {
        borderEdges += edge;
        ++borderEdgeCount;
      } else {
        centerEdges += edge;
        ++centerEdgeCount;
      }
    }
  }
  const double averageBand =
      bandCount > 0 ? bandLuma / static_cast<double>(bandCount) : 0.0;
  const double averageCenter =
      centerCount > 0 ? centerLuma / static_cast<double>(centerCount) : 0.0;
  const float darkBandScore =
      bandCount > 0 ? std::clamp((static_cast<float>(bandDark) /
                                      static_cast<float>(bandCount) -
                                  0.68f) /
                                     0.32f,
                                 0.0f, 1.0f)
                    : 0.0f;
  const float contrastScore = static_cast<float>(
      std::clamp((averageCenter - averageBand - 12.0) / 54.0, 0.0, 1.0));
  out.letterboxConfidence = darkBandScore * contrastScore;
  out.borderEdgeDensity =
      borderEdgeCount > 0
          ? static_cast<float>(borderEdges /
                               static_cast<double>(borderEdgeCount))
          : 0.0f;
  out.centerEdgeDensity =
      centerEdgeCount > 0
          ? static_cast<float>(centerEdges /
                               static_cast<double>(centerEdgeCount))
          : 0.0f;
  *sample = std::move(out);
  return true;
}

} // namespace

VisualTimelineScanResult
scanVisualTimeline(const VisualTimelineScanRequest &request,
                   const VisualTimelineScanControl &control,
                   VisualTimelineScanCheckpoint *checkpoint) {
  VisualTimelineScanCheckpoint localCheckpoint;
  VisualTimelineScanCheckpoint *state =
      checkpoint ? checkpoint : &localCheckpoint;
  if (request.videoPath.empty()) {
    return {VisualTimelineScanStatus::Unsupported,
            "Video analysis requires a source path.",
            0,
            {}};
  }
  if (!validVisualTimelineScanCheckpoint(request, *state)) {
    return {VisualTimelineScanStatus::Failed,
            "The retained temporal video scan is invalid.",
            0,
            {}};
  }
  if (interruption(control) != Interruption::None) {
    return finishInterruption(control, state);
  }

  VideoDecoder decoder;
  std::string decoderError;
  DecoderInterruptContext interruptContext{&control};
  if (!decoder.init(request.videoPath, &decoderError, true, true, nullptr,
                    request.videoStreamIndex, &interruptDecoder,
                    &interruptContext, VideoCpuOutputPrecision::EightBit,
                    request.requireD3d11
                        ? VideoHardwareDecodePolicy::RequireD3D11
                        : VideoHardwareDecodePolicy::AllowSoftwareFallback)) {
    if (interruption(control) != Interruption::None) {
      return finishInterruption(control, state);
    }
    *state = {};
    return {request.requireD3d11 ? VisualTimelineScanStatus::Unsupported
                                 : VisualTimelineScanStatus::Failed,
            decoderError.empty() ? "Could not open the video stream."
                                 : std::move(decoderError),
            0,
            {}};
  }
  if (!decoder.setTargetSize(kDecodeWidth, kDecodeHeight, &decoderError)) {
    if (interruption(control) != Interruption::None) {
      return finishInterruption(control, state);
    }
    *state = {};
    return {request.requireD3d11 ? VisualTimelineScanStatus::Unsupported
                                 : VisualTimelineScanStatus::Failed,
            decoderError.empty() ? "Could not configure temporal video samples."
                                 : std::move(decoderError),
            0,
            {}};
  }

  const std::int64_t decoderDurationUs =
      std::max<std::int64_t>(0, decoder.duration100ns() / 10);
  const std::int64_t durationUs = request.expectedDurationUs > 0
                                      ? request.expectedDurationUs
                                      : decoderDurationUs;
  if (durationUs <= 0) {
    *state = {};
    return {VisualTimelineScanStatus::Unsupported,
            "Video analysis requires a known duration.",
            0,
            {}};
  }

  if (!state->initialized) {
    state->initialized = true;
    state->videoPath = request.videoPath;
    state->videoStreamIndex = request.videoStreamIndex;
    state->expectedDurationUs = request.expectedDurationUs;
    state->requireD3d11 = request.requireD3d11;
    state->durationUs = durationUs;
    state->nextSampleUs = 0;
  } else if (state->durationUs != durationUs) {
    *state = {};
    return {VisualTimelineScanStatus::Failed,
            "The video duration changed while its temporal scan was paused.",
            0,
            {}};
  }

  VisualTimelineScanResult result;
  result.durationUs = durationUs;
  try {
    state->samples.reserve(static_cast<std::size_t>(std::min<std::int64_t>(
        durationUs / kVisualSampleIntervalUs + 2, 100'000)));
  } catch (const std::bad_alloc &) {
    *state = {};
    return {VisualTimelineScanStatus::Failed,
            "Could not allocate the temporal video scan.",
            durationUs,
            {}};
  }
  std::int64_t nextSampleUs = state->nextSampleUs;
  if (nextSampleUs > 0) {
    // Seek is backward/keyframe based, then the loop discards frames before
    // nextSampleUs. If the last retained sample was at the duration boundary,
    // seek into the short tail instead of accidentally rescanning from zero.
    const std::int64_t resumeTargetUs = std::min(
        nextSampleUs,
        std::max<std::int64_t>(0, durationUs - kVisualSampleIntervalUs));
    if (resumeTargetUs > (std::numeric_limits<std::int64_t>::max)() / 10 ||
        !decoder.seekToTimestamp100ns(resumeTargetUs * 10)) {
      if (interruption(control) != Interruption::None) {
        return finishInterruption(control, state, durationUs);
      }
      *state = {};
      return {VisualTimelineScanStatus::Failed,
              "Could not resume the temporal video scan.",
              durationUs,
              {}};
    }
  }
  while (interruption(control) == Interruption::None) {
    VideoFrame timingFrame;
    if (!decoder.readFrame(timingFrame, nullptr, false))
      break;
    const std::int64_t timestampUs =
        std::max<std::int64_t>(0, timingFrame.timestamp100ns / 10);
    if (timestampUs < nextSampleUs)
      continue;

    VideoFrame pixels;
    VisualSample sample;
    if (!decoder.redecodeLastFrame(pixels) ||
        !extractVisualSample(pixels, timestampUs, &sample)) {
      if (interruption(control) != Interruption::None) {
        return finishInterruption(control, state, durationUs);
      }
      *state = {};
      return {VisualTimelineScanStatus::Failed,
              "Could not read a temporal video sample.",
              durationUs,
              {}};
    }
    if (!state->samples.empty()) {
      sample.changeScore = visualChangeScore(state->samples.back(), sample);
    }
    state->samples.push_back(std::move(sample));
    do {
      if (nextSampleUs > (std::numeric_limits<std::int64_t>::max)() -
                             kVisualSampleIntervalUs) {
        nextSampleUs = (std::numeric_limits<std::int64_t>::max)();
        break;
      }
      nextSampleUs += kVisualSampleIntervalUs;
    } while (nextSampleUs <= timestampUs);
    state->nextSampleUs = nextSampleUs;
    if (!report(control,
                static_cast<double>(std::min(timestampUs, durationUs)) /
                    static_cast<double>(durationUs))) {
      *state = {};
      return {VisualTimelineScanStatus::Failed,
              "Could not publish video-analysis progress.",
              durationUs,
              {}};
    }
  }
  if (interruption(control) != Interruption::None) {
    return finishInterruption(control, state, durationUs);
  }
  if (!decoder.reachedEndOfStream()) {
    *state = {};
    return {VisualTimelineScanStatus::Failed,
            "Video decoding stopped before the temporal scan completed.",
            0,
            {}};
  }
  if (state->samples.size() < 2) {
    *state = {};
    return {VisualTimelineScanStatus::Unsupported,
            "The video contains too few decodable temporal samples.",
            0,
            {}};
  }
  if (!report(control, 1.0)) {
    *state = {};
    return {VisualTimelineScanStatus::Failed,
            "Could not publish video-analysis progress.",
            0,
            {}};
  }
  result.status = VisualTimelineScanStatus::Succeeded;
  result.samples = std::move(state->samples);
  *state = {};
  return result;
}

} // namespace playback_video_analysis
