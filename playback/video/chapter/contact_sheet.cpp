#include "playback/video/chapter/contact_sheet.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>
#include <utility>

#include "playback/video/decoder.h"
#include "playback/video/image.h"
#include "playback/video/timeline_preview_decoder.h"

namespace playback_video_chapters {
namespace {

constexpr int kSampleCount = 12;
constexpr int kSheetColumns = 4;
constexpr int kSheetRows = 3;
constexpr int kTileWidth = 384;
constexpr int kTileHeight = 216;
constexpr int kMaximumDecodeFrames = 2048;
constexpr auto kSampleDeadline = std::chrono::seconds(8);

std::int64_t steadyNowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct InterruptContext {
  const OperationControl* control = nullptr;
  std::int64_t deadlineUs = 0;

  static int callback(void* opaque) {
    const auto* self = static_cast<const InterruptContext*>(opaque);
    if (!self || !self->control) return 1;
    try {
      if (self->control->cancelled && self->control->cancelled()) return 1;
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

OperationStatus interruptionStatus(const OperationControl& control) {
  if (control.cancelled && control.cancelled()) {
    return OperationStatus::Cancelled;
  }
  if (control.backgroundGpuAllowed && !control.backgroundGpuAllowed()) {
    return OperationStatus::Yielded;
  }
  return OperationStatus::Failed;
}

OperationStatus initializeHardwareDecoder(
    const AnalysisRequest& request, const OperationControl& control,
    InterruptContext* interrupt, VideoDecoder* decoder,
    std::string* detail) {
  if (!interrupt || !decoder) return OperationStatus::Failed;
  std::string decoderError;
  if (!decoder->init(request.file, &decoderError, true, true, nullptr,
                     request.videoStreamIndex,
                     &InterruptContext::callback, interrupt,
                     VideoCpuOutputPrecision::EightBit,
                     VideoHardwareDecodePolicy::RequireD3D11)) {
    const OperationStatus interrupted = interruptionStatus(control);
    if (interrupted == OperationStatus::Cancelled) {
      if (detail) *detail = "Chapter analysis cancelled.";
      return interrupted;
    }
    if (interrupted == OperationStatus::Yielded) {
      if (detail) *detail = "Playback reclaimed the GPU.";
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
  const double scale = std::min(
      static_cast<double>(kTileWidth) / sourceWidth,
      static_cast<double>(kTileHeight) / sourceHeight);
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
  if (detail) detail->clear();
  return OperationStatus::Succeeded;
}

bool sampleFrame(VideoDecoder* decoder, InterruptContext* interrupt,
                 std::int64_t targetUs,
                 playback_video_image::RgbaImage* image) {
  if (!decoder || !interrupt || !image || targetUs < 0) return false;
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
    if (InterruptContext::callback(interrupt)) return false;
    VideoFrame frame;
    if (!decoder->readFrame(frame, nullptr, false)) break;
    const std::int64_t ptsUs = std::max<std::int64_t>(0,
                                                       frame.timestamp100ns / 10);
    std::int64_t durationUs = frame.duration100ns / 10;
    if (durationUs <= 0) durationUs = 33333;
    haveCandidate = true;
    if (ptsUs >= targetUs ||
        (ptsUs <= targetUs && durationUs > targetUs - ptsUs)) {
      reachedTarget = true;
      break;
    }
  }
  VideoFrame selected;
  return haveCandidate &&
         (reachedTarget || decoder->reachedEndOfStream()) &&
         !InterruptContext::callback(interrupt) &&
         decoder->redecodeLastFrame(selected) &&
         playback_video_timeline_preview::convertFrameToRgba(selected,
                                                              image);
}

bool copyTile(const playback_video_image::RgbaImage& tile, int column,
              int row, ContactSheetResult* sheet) {
  if (!sheet || !playback_video_image::validate(tile) || sheet->rgb.empty() ||
      tile.width > kTileWidth || tile.height > kTileHeight) {
    return false;
  }
  const int insetX = (kTileWidth - static_cast<int>(tile.width)) / 2;
  const int insetY = (kTileHeight - static_cast<int>(tile.height)) / 2;
  for (std::uint32_t y = 0; y < tile.height; ++y) {
    const std::uint8_t* source =
        tile.pixels.data() + static_cast<std::size_t>(y) * tile.strideBytes;
    std::uint8_t* destination =
        sheet->rgb.data() +
        static_cast<std::size_t>(row * kTileHeight + insetY +
                                 static_cast<int>(y)) *
            static_cast<std::size_t>(sheet->width) * 3u +
        static_cast<std::size_t>(column * kTileWidth + insetX) * 3u;
    for (std::uint32_t x = 0; x < tile.width; ++x) {
      destination[x * 3u + 0u] = source[x * 4u + 0u];
      destination[x * 3u + 1u] = source[x * 4u + 1u];
      destination[x * 3u + 2u] = source[x * 4u + 2u];
    }
  }
  return true;
}

}  // namespace

OperationStatus probeHardwareVideoDecode(const AnalysisRequest& request,
                                         const OperationControl& control,
                                         std::string* detail) {
  if (detail) detail->clear();
  if (request.file.empty() || request.videoStreamIndex < 0 ||
      request.durationUs <= 0) {
    if (detail) {
      *detail = "The video cannot be sampled for chapter analysis.";
    }
    return OperationStatus::Unsupported;
  }
  if (control.backgroundGpuAllowed && !control.backgroundGpuAllowed()) {
    if (detail) *detail = "Playback has GPU priority.";
    return OperationStatus::Yielded;
  }
  if (control.progress) {
    control.progress(std::nullopt, "Testing D3D11 hardware video decode");
  }
  InterruptContext interrupt{&control, 0};
  VideoDecoder decoder;
  OperationStatus status = initializeHardwareDecoder(
      request, control, &interrupt, &decoder, detail);
  if (status != OperationStatus::Succeeded) return status;
  playback_video_image::RgbaImage sample;
  if (sampleFrame(&decoder, &interrupt, 0, &sample)) {
    return OperationStatus::Succeeded;
  }
  status = interruptionStatus(control);
  if (status == OperationStatus::Cancelled) {
    if (detail) *detail = "Chapter analysis cancelled.";
    return status;
  }
  if (status == OperationStatus::Yielded) {
    if (detail) *detail = "Playback reclaimed the GPU.";
    return status;
  }
  if (detail) {
    *detail = "This video cannot produce a D3D11 hardware-decoded sample.";
  }
  return OperationStatus::Unsupported;
}

ContactSheetResult buildContactSheet(const AnalysisRequest& request,
                                     const OperationControl& control) {
  ContactSheetResult result;
  if (request.file.empty() || request.videoStreamIndex < 0 ||
      request.durationUs <= 0) {
    result.detail = "The video cannot be sampled for chapter analysis.";
    return result;
  }
  if (control.backgroundGpuAllowed && !control.backgroundGpuAllowed()) {
    result.status = OperationStatus::Yielded;
    result.detail = "Playback has GPU priority.";
    return result;
  }

  InterruptContext interrupt{&control, 0};
  VideoDecoder decoder;
  result.status = initializeHardwareDecoder(
      request, control, &interrupt, &decoder, &result.detail);
  if (result.status != OperationStatus::Succeeded) return result;

  result.width = kSheetColumns * kTileWidth;
  result.height = kSheetRows * kTileHeight;
  if (result.width >
      (std::numeric_limits<std::size_t>::max)() / result.height / 3u) {
    result.detail = "The chapter contact sheet is too large.";
    return result;
  }
  try {
    result.rgb.assign(static_cast<std::size_t>(result.width) * result.height *
                          3u,
                      0u);
  } catch (const std::bad_alloc&) {
    result.detail = "Could not allocate the chapter contact sheet.";
    return result;
  }

  result.sampleTimesUs.reserve(kSampleCount);
  for (int index = 0; index < kSampleCount; ++index) {
    if ((control.cancelled && control.cancelled()) ||
        (control.backgroundGpuAllowed &&
         !control.backgroundGpuAllowed())) {
      result.status = interruptionStatus(control);
      result.detail = result.status == OperationStatus::Yielded
                          ? "Playback reclaimed the GPU."
                          : "Chapter analysis cancelled.";
      return result;
    }
    const std::int64_t targetUs =
        index == kSampleCount - 1
            ? request.durationUs - 1
            : static_cast<std::int64_t>(
                  (static_cast<long double>(request.durationUs - 1) * index) /
                  (kSampleCount - 1));
    playback_video_image::RgbaImage tile;
    if (!sampleFrame(&decoder, &interrupt, targetUs, &tile) ||
        !copyTile(tile, index % kSheetColumns, index / kSheetColumns,
                  &result)) {
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
    result.sampleTimesUs.push_back(targetUs);
    if (control.progress) {
      control.progress(0.05 + 0.50 *
                                  static_cast<double>(index + 1) /
                                  kSampleCount,
                       "Sampling video on D3D11");
    }
  }
  decoder.uninit();
  result.status = OperationStatus::Succeeded;
  return result;
}

}  // namespace playback_video_chapters
