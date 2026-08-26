#include "playback/video/analysis/scene_analyzer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "playback/video/decoder.h"
#include "playback/video/analysis/scene_analysis_cache.h"
#include "playback/video/transcript/document.h"
#include "playback/video/transcript/artifact.h"

namespace playback_video_analysis {
namespace {

constexpr int kDecodeWidth = 192;
constexpr int kDecodeHeight = 108;

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

bool cancelled(const std::atomic<bool>* cancelRequested) {
  return cancelRequested &&
         cancelRequested->load(std::memory_order_relaxed);
}

struct DecoderInterruptContext {
  const std::atomic<bool>* cancelRequested = nullptr;
};

int interruptDecoder(void* opaque) {
  const auto* context =
      static_cast<const DecoderInterruptContext*>(opaque);
  return !context || cancelled(context->cancelRequested) ? 1 : 0;
}

void report(const AnalysisProgressCallback& callback, double fraction,
            std::string phase) {
  if (!callback) return;
  callback({std::clamp(fraction, 0.0, 1.0), std::move(phase)});
}

bool frameLumaAt(const VideoFrame& frame, int x, int y, uint8_t* luma) {
  if (!luma || frame.width <= 0 || frame.height <= 0 || x < 0 || y < 0 ||
      x >= frame.width || y >= frame.height || frame.stride <= 0) {
    return false;
  }
  const size_t stride = static_cast<size_t>(frame.stride);
  if (frame.format == VideoPixelFormat::NV12) {
    const size_t offset = static_cast<size_t>(y) * stride +
                          static_cast<size_t>(x);
    if (offset >= frame.yuv.size()) return false;
    *luma = frame.yuv[offset];
    return true;
  }
  if (frame.format == VideoPixelFormat::P010) {
    const size_t offset = static_cast<size_t>(y) * stride +
                          static_cast<size_t>(x) * 2u;
    if (offset + sizeof(uint16_t) > frame.yuv.size()) return false;
    uint16_t value = 0;
    std::memcpy(&value, frame.yuv.data() + offset, sizeof(value));
    *luma = static_cast<uint8_t>(std::min<uint16_t>(255, value >> 8));
    return true;
  }
  if (frame.format == VideoPixelFormat::RGB32 ||
      frame.format == VideoPixelFormat::ARGB32) {
    const size_t offset = static_cast<size_t>(y) * stride +
                          static_cast<size_t>(x) * 4u;
    if (offset + 3u > frame.rgba.size()) return false;
    const unsigned int first = frame.rgba[offset];
    const unsigned int second = frame.rgba[offset + 1u];
    const unsigned int third = frame.rgba[offset + 2u];
    // Channel order differs between a few FFmpeg output paths. A symmetric
    // average is sufficient for scene signatures and avoids assigning a
    // colour-space contract to VideoFrame that its playback users do not need.
    *luma = static_cast<uint8_t>((first + second + third) / 3u);
    return true;
  }
  return false;
}

bool extractVisualSample(const VideoFrame& frame, int64_t timestampUs,
                         VisualSample* sample) {
  if (!sample || frame.width <= 0 || frame.height <= 0) return false;
  VisualSample out;
  out.timestampUs = std::max<int64_t>(0, timestampUs);
  double lumaTotal = 0.0;
  size_t darkCount = 0;
  for (int gridY = 0; gridY < kFeatureGridRows; ++gridY) {
    const int y = std::clamp(
        static_cast<int>((static_cast<int64_t>(gridY * 2 + 1) *
                          frame.height) /
                         (kFeatureGridRows * 2)),
        0, frame.height - 1);
    for (int gridX = 0; gridX < kFeatureGridColumns; ++gridX) {
      const int x = std::clamp(
          static_cast<int>((static_cast<int64_t>(gridX * 2 + 1) *
                            frame.width) /
                           (kFeatureGridColumns * 2)),
          0, frame.width - 1);
      uint8_t value = 0;
      if (!frameLumaAt(frame, x, y, &value)) return false;
      const size_t index = static_cast<size_t>(
          gridY * kFeatureGridColumns + gridX);
      out.luma[index] = value;
      lumaTotal += value;
      if (value < 24) ++darkCount;
    }
  }
  out.meanLuma = static_cast<float>(
      lumaTotal / static_cast<double>(kFeatureGridSize * 255));
  out.darkFraction = static_cast<float>(darkCount) /
                     static_cast<float>(kFeatureGridSize);

  double bandLuma = 0.0;
  double centerLuma = 0.0;
  size_t bandDark = 0;
  size_t bandCount = 0;
  size_t centerCount = 0;
  double borderEdges = 0.0;
  double centerEdges = 0.0;
  size_t borderEdgeCount = 0;
  size_t centerEdgeCount = 0;
  for (int y = 0; y < kFeatureGridRows; ++y) {
    for (int x = 0; x < kFeatureGridColumns; ++x) {
      const size_t index =
          static_cast<size_t>(y * kFeatureGridColumns + x);
      const uint8_t value = out.luma[index];
      const bool inBand = y < 2 || y >= kFeatureGridRows - 2;
      if (inBand) {
        bandLuma += value;
        ++bandCount;
        if (value < 20) ++bandDark;
      } else {
        centerLuma += value;
        ++centerCount;
      }
      if (x + 1 >= kFeatureGridColumns) continue;
      const double edge =
          std::abs(static_cast<int>(value) -
                   static_cast<int>(out.luma[index + 1u])) /
          255.0;
      const bool border = x < 6 || x >= kFeatureGridColumns - 7 ||
                          y < 3 || y >= kFeatureGridRows - 3;
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
      bandCount > 0
          ? std::clamp((static_cast<float>(bandDark) /
                            static_cast<float>(bandCount) -
                        0.68f) /
                           0.32f,
                       0.0f, 1.0f)
          : 0.0f;
  const float contrastScore = static_cast<float>(std::clamp(
      (averageCenter - averageBand - 12.0) / 54.0, 0.0, 1.0));
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

}  // namespace

bool analyzeVideoScenes(const std::filesystem::path& videoPath,
                        int videoStreamIndex, int64_t expectedDurationUs,
                        const AnalysisProgressCallback& onProgress,
                        const std::atomic<bool>* cancelRequested,
                        bool allowCachedResult,
                        AnalysisResult* result, std::string* error) {
  if (error) error->clear();
  if (result) *result = {};
  if (videoPath.empty() || !result) {
    setError(error, "Video path or analysis result is empty.");
    return false;
  }
  if (cancelled(cancelRequested)) {
    setError(error, "Scene analysis cancelled.");
    return false;
  }

  const std::filesystem::path transcriptPath =
      playback_video_transcript::activeTranscriptPathForVideo(videoPath);
  if (allowCachedResult && expectedDurationUs > 0 &&
      loadCachedSceneAnalysis(videoPath, videoStreamIndex,
                              expectedDurationUs, transcriptPath, result)) {
    report(onProgress, 1.0, "Loaded cached scene analysis");
    return true;
  }

  report(onProgress, 0.01, "Opening video analysis stream");
  VideoDecoder decoder;
  std::string decoderError;
  DecoderInterruptContext interruptContext{cancelRequested};
  if (!decoder.init(videoPath, &decoderError, true, true, nullptr,
                    videoStreamIndex, &interruptDecoder,
                    &interruptContext)) {
    setError(error, decoderError.empty() ? "Could not open the video stream."
                                         : std::move(decoderError));
    return false;
  }
  if (!decoder.setTargetSize(kDecodeWidth, kDecodeHeight, &decoderError)) {
    setError(error, decoderError.empty() ? "Could not configure scene samples."
                                         : std::move(decoderError));
    return false;
  }

  const int64_t decoderDurationUs =
      std::max<int64_t>(0, decoder.duration100ns() / 10);
  const int64_t durationUs =
      expectedDurationUs > 0 ? expectedDurationUs : decoderDurationUs;
  if (durationUs <= 0) {
    setError(error, "Scene analysis requires a known video duration.");
    return false;
  }
  if (allowCachedResult && expectedDurationUs <= 0 &&
      loadCachedSceneAnalysis(videoPath, videoStreamIndex, durationUs,
                              transcriptPath, result)) {
    report(onProgress, 1.0, "Loaded cached scene analysis");
    return true;
  }

  std::vector<VisualSample> samples;
  samples.reserve(static_cast<size_t>(
      std::min<int64_t>(durationUs / kVisualSampleIntervalUs + 2,
                        100'000)));
  int64_t nextSampleUs = 0;
  while (!cancelled(cancelRequested)) {
    VideoFrame timingFrame;
    if (!decoder.readFrame(timingFrame, nullptr, false)) break;
    const int64_t timestampUs =
        std::max<int64_t>(0, timingFrame.timestamp100ns / 10);
    if (timestampUs < nextSampleUs) continue;

    VideoFrame pixels;
    VisualSample sample;
    if (!decoder.redecodeLastFrame(pixels) ||
        !extractVisualSample(pixels, timestampUs, &sample)) {
      setError(error, "Could not transfer a scene-analysis video sample.");
      return false;
    }
    if (!samples.empty()) {
      sample.changeScore = visualChangeScore(samples.back(), sample);
    }
    samples.push_back(std::move(sample));
    do {
      if (nextSampleUs >
          (std::numeric_limits<int64_t>::max)() -
              kVisualSampleIntervalUs) {
        nextSampleUs = (std::numeric_limits<int64_t>::max)();
        break;
      }
      nextSampleUs += kVisualSampleIntervalUs;
    } while (nextSampleUs <= timestampUs);
    const double fraction =
        static_cast<double>(std::min(timestampUs, durationUs)) /
        static_cast<double>(durationUs);
    report(onProgress, 0.03 + fraction * 0.87,
           "Scanning visual scene changes");
  }
  if (cancelled(cancelRequested)) {
    setError(error, "Scene analysis cancelled.");
    return false;
  }
  if (!decoder.reachedEndOfStream()) {
    setError(error, "Video decoding stopped before scene analysis completed.");
    return false;
  }
  if (samples.size() < 2) {
    setError(error, "The video contains too few decodable scene samples.");
    return false;
  }

  report(onProgress, 0.92, "Reading transcript evidence");
  std::vector<playback_video_transcript::Segment> transcriptSegments;
  if (!transcriptPath.empty()) {
    std::string transcriptError;
    if (!playback_video_transcript::readIndexedTranscript(
            transcriptPath, &transcriptSegments, &transcriptError)) {
      transcriptSegments.clear();
    }
  }
  const std::vector<SpeechActivity> speech =
      buildSpeechActivity(transcriptSegments);
  report(onProgress, 0.96, "Grouping chapters and cutscene candidates");
  std::vector<SceneSuggestion> suggestions =
      buildSceneSuggestions(durationUs, samples, speech);
  if (suggestions.empty()) {
    setError(error, "Scene analysis produced no usable ranges.");
    return false;
  }

  result->durationUs = durationUs;
  result->visualSampleCount = samples.size();
  result->transcriptPath =
      transcriptSegments.empty() ? std::filesystem::path{} : transcriptPath;
  result->suggestions = std::move(suggestions);
  storeCachedSceneAnalysis(videoPath, videoStreamIndex, durationUs,
                           transcriptPath, *result);
  report(onProgress, 1.0, "Scene analysis complete");
  return true;
}

}  // namespace playback_video_analysis
