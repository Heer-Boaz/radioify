#include "playback/video/analysis/scene_analysis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>

#include "playback/video/transcript/cue_semantics.h"

namespace playback_video_analysis {
namespace {

constexpr int64_t kWindowUs = 12'000'000;
constexpr int64_t kMinimumSplitUs = 30'000'000;
constexpr int64_t kTargetGameplayChapterUs = 150'000'000;
constexpr int64_t kMaximumGameplayChapterUs = 210'000'000;
constexpr int64_t kMaximumOtherChapterUs = 300'000'000;

float clampUnit(float value) { return std::clamp(value, 0.0f, 1.0f); }

float median(std::vector<float> values) {
  if (values.empty()) return 0.0f;
  const size_t middle = values.size() / 2;
  std::nth_element(values.begin(), values.begin() +
                                      static_cast<ptrdiff_t>(middle),
                   values.end());
  const float upper = values[middle];
  if ((values.size() & 1u) != 0u) return upper;
  std::nth_element(values.begin(),
                   values.begin() + static_cast<ptrdiff_t>(middle - 1),
                   values.begin() + static_cast<ptrdiff_t>(middle));
  return (values[middle - 1] + upper) * 0.5f;
}

float adaptiveCutThreshold(const std::vector<VisualSample>& samples) {
  std::vector<float> changes;
  changes.reserve(samples.size());
  for (size_t index = 1; index < samples.size(); ++index) {
    changes.push_back(samples[index].changeScore);
  }
  const float center = median(changes);
  for (float& value : changes) value = std::abs(value - center);
  const float deviation = median(std::move(changes));
  return std::clamp(center + std::max(0.08f, deviation * 3.5f), 0.18f,
                    0.52f);
}

struct WindowObservation {
  int64_t startUs = 0;
  int64_t endUs = 0;
  SceneEvidence evidence;
  float cutsceneScore = 0.0f;
  float dialogueScore = 0.0f;
  float menuScore = 0.0f;
  SceneKind kind = SceneKind::Gameplay;
};

float speechRatioForRange(int64_t startUs, int64_t endUs,
                          const std::vector<SpeechActivity>& speech) {
  if (endUs <= startUs) return 0.0f;
  std::vector<std::pair<int64_t, int64_t>> intersections;
  for (const SpeechActivity& activity : speech) {
    if (activity.endUs <= startUs) continue;
    if (activity.startUs >= endUs) break;
    const int64_t begin = std::max(startUs, activity.startUs);
    const int64_t end = std::min(endUs, activity.endUs);
    if (end > begin) intersections.emplace_back(begin, end);
  }
  if (intersections.empty()) return 0.0f;
  std::sort(intersections.begin(), intersections.end());
  int64_t coveredUs = 0;
  int64_t currentStart = intersections.front().first;
  int64_t currentEnd = intersections.front().second;
  for (size_t index = 1; index < intersections.size(); ++index) {
    if (intersections[index].first <= currentEnd) {
      currentEnd = std::max(currentEnd, intersections[index].second);
    } else {
      coveredUs += currentEnd - currentStart;
      currentStart = intersections[index].first;
      currentEnd = intersections[index].second;
    }
  }
  coveredUs += currentEnd - currentStart;
  return clampUnit(static_cast<float>(coveredUs) /
                   static_cast<float>(endUs - startUs));
}

WindowObservation observeWindow(
    int64_t startUs, int64_t endUs,
    const std::vector<VisualSample>& samples,
    const std::vector<SpeechActivity>& speech, float cutThreshold) {
  WindowObservation out;
  out.startUs = startUs;
  out.endUs = endUs;
  double dark = 0.0;
  double letterbox = 0.0;
  double borderEdges = 0.0;
  double centerEdges = 0.0;
  size_t sampleCount = 0;
  size_t changeObservationCount = 0;
  size_t changeCount = 0;
  double visualMotion = 0.0;
  for (const VisualSample& sample : samples) {
    if (sample.timestampUs < startUs) continue;
    if (sample.timestampUs >= endUs) break;
    dark += sample.darkFraction;
    letterbox += sample.letterboxConfidence;
    borderEdges += sample.borderEdgeDensity;
    centerEdges += sample.centerEdgeDensity;
    // A sample's change score describes its transition from the preceding
    // sample. The first sample in a window therefore belongs to the boundary,
    // not to motion inside the newly classified range.
    if (sampleCount > 0) {
      if (sample.changeScore >= cutThreshold) ++changeCount;
      visualMotion += sample.changeScore;
      ++changeObservationCount;
    }
    ++sampleCount;
  }
  if (sampleCount > 0) {
    const double count = static_cast<double>(sampleCount);
    out.evidence.darkRatio = static_cast<float>(dark / count);
    out.evidence.letterboxRatio = static_cast<float>(letterbox / count);
    const double changeDivisor = static_cast<double>(
        std::max<size_t>(1, changeObservationCount));
    out.evidence.sceneChangeRate =
        static_cast<float>(changeCount / changeDivisor);
    out.evidence.visualMotion =
        static_cast<float>(visualMotion / changeDivisor);
  }
  out.evidence.speechRatio = speechRatioForRange(startUs, endUs, speech);

  const float speechScore =
      clampUnit(out.evidence.speechRatio / 0.24f);
  const float cutRateScore =
      clampUnit(out.evidence.sceneChangeRate / 0.24f);
  float hudLikelihood = 0.0f;
  if (sampleCount > 0) {
    const float averageBorder =
        static_cast<float>(borderEdges / static_cast<double>(sampleCount));
    const float averageCenter =
        static_cast<float>(centerEdges / static_cast<double>(sampleCount));
    hudLikelihood = clampUnit((averageBorder - averageCenter * 0.8f) / 0.16f);
  }
  out.evidence.hudLikelihood = hudLikelihood;

  out.cutsceneScore = clampUnit(
      out.evidence.letterboxRatio * 0.55f + speechScore * 0.28f +
      cutRateScore * 0.12f + (1.0f - hudLikelihood) * 0.05f);
  out.dialogueScore = clampUnit(speechScore * 0.88f +
                                out.evidence.letterboxRatio * 0.12f);
  const float darkScore = clampUnit(out.evidence.darkRatio / 0.68f);
  const float staticScore =
      1.0f - clampUnit(out.evidence.visualMotion / 0.10f);
  out.menuScore = clampUnit(darkScore * 0.67f + staticScore * 0.25f +
                            (1.0f - speechScore) * 0.08f -
                            hudLikelihood * 0.38f);

  if (out.evidence.darkRatio >= 0.64f && out.menuScore >= 0.60f &&
      out.evidence.speechRatio < 0.06f && staticScore >= 0.48f &&
      hudLikelihood < 0.42f) {
    out.kind = SceneKind::MenuOrLoading;
  } else if (out.cutsceneScore >= 0.52f) {
    out.kind = SceneKind::Cutscene;
  } else if (out.dialogueScore >= 0.34f) {
    out.kind = SceneKind::Dialogue;
  } else {
    out.kind = SceneKind::Gameplay;
  }
  return out;
}

void smoothWindowKinds(std::vector<WindowObservation>* windows) {
  if (!windows || windows->size() < 3) return;
  std::vector<SceneKind> smoothed;
  smoothed.reserve(windows->size());
  for (const WindowObservation& window : *windows) {
    smoothed.push_back(window.kind);
  }
  for (size_t index = 1; index + 1 < windows->size(); ++index) {
    const SceneKind previous = (*windows)[index - 1].kind;
    const SceneKind current = (*windows)[index].kind;
    const SceneKind next = (*windows)[index + 1].kind;
    if (previous == next && current != previous) {
      const bool preserveLoading =
          current == SceneKind::MenuOrLoading &&
          (*windows)[index].evidence.darkRatio >= 0.78f;
      if (!preserveLoading) smoothed[index] = previous;
    } else if (current == SceneKind::Dialogue &&
               (previous == SceneKind::Cutscene ||
                next == SceneKind::Cutscene) &&
               (*windows)[index].cutsceneScore >= 0.40f) {
      smoothed[index] = SceneKind::Cutscene;
    }
  }
  for (size_t index = 0; index < windows->size(); ++index) {
    (*windows)[index].kind = smoothed[index];
  }
}

std::optional<int64_t> strongestBoundaryNear(
    int64_t nominalUs, int64_t minimumUs, int64_t maximumUs,
    const std::vector<VisualSample>& samples) {
  constexpr int64_t kSearchRadiusUs = 4'000'000;
  const int64_t searchStart =
      std::max(minimumUs, nominalUs - kSearchRadiusUs);
  const int64_t searchEnd =
      std::min(maximumUs, nominalUs + kSearchRadiusUs);
  const VisualSample* best = nullptr;
  for (const VisualSample& sample : samples) {
    if (sample.timestampUs < searchStart) continue;
    if (sample.timestampUs > searchEnd) break;
    if (!best || sample.changeScore > best->changeScore) best = &sample;
  }
  return best ? std::optional<int64_t>(best->timestampUs) : std::nullopt;
}

SceneEvidence evidenceForRange(
    int64_t startUs, int64_t endUs,
    const std::vector<WindowObservation>& windows) {
  SceneEvidence out;
  double weightTotal = 0.0;
  for (const WindowObservation& window : windows) {
    const int64_t overlap =
        std::min(endUs, window.endUs) - std::max(startUs, window.startUs);
    if (overlap <= 0) continue;
    const double weight = static_cast<double>(overlap);
    out.speechRatio +=
        static_cast<float>(window.evidence.speechRatio * weight);
    out.letterboxRatio +=
        static_cast<float>(window.evidence.letterboxRatio * weight);
    out.darkRatio += static_cast<float>(window.evidence.darkRatio * weight);
    out.sceneChangeRate +=
        static_cast<float>(window.evidence.sceneChangeRate * weight);
    out.visualMotion +=
        static_cast<float>(window.evidence.visualMotion * weight);
    out.hudLikelihood +=
        static_cast<float>(window.evidence.hudLikelihood * weight);
    weightTotal += weight;
  }
  if (weightTotal > 0.0) {
    const float divisor = static_cast<float>(weightTotal);
    out.speechRatio /= divisor;
    out.letterboxRatio /= divisor;
    out.darkRatio /= divisor;
    out.sceneChangeRate /= divisor;
    out.visualMotion /= divisor;
    out.hudLikelihood /= divisor;
  }
  return out;
}

float confidenceFor(SceneKind kind, const SceneEvidence& evidence) {
  const float speech = clampUnit(evidence.speechRatio / 0.24f);
  const float cuts = clampUnit(evidence.sceneChangeRate / 0.24f);
  const float cutscene = clampUnit(evidence.letterboxRatio * 0.58f +
                                   speech * 0.30f + cuts * 0.12f);
  const float menu = clampUnit(
      evidence.darkRatio * 0.68f +
      (1.0f - clampUnit(evidence.visualMotion / 0.10f)) * 0.22f +
      (1.0f - evidence.hudLikelihood) * 0.10f);
  switch (kind) {
    case SceneKind::Cutscene:
      return std::clamp(0.48f + cutscene * 0.48f, 0.48f, 0.97f);
    case SceneKind::Dialogue:
      return std::clamp(0.42f + speech * 0.46f, 0.42f, 0.91f);
    case SceneKind::MenuOrLoading:
      return std::clamp(0.48f + menu * 0.48f, 0.48f, 0.97f);
    case SceneKind::Gameplay:
      return std::clamp(0.45f + (1.0f - std::max(cutscene, menu)) * 0.42f,
                        0.45f, 0.90f);
  }
  return 0.0f;
}

struct ClassifiedRange {
  int64_t startUs = 0;
  int64_t endUs = 0;
  SceneKind kind = SceneKind::Gameplay;
};

std::vector<ClassifiedRange> mergeWindows(
    int64_t durationUs, const std::vector<WindowObservation>& windows,
    const std::vector<VisualSample>& samples) {
  std::vector<ClassifiedRange> ranges;
  for (const WindowObservation& window : windows) {
    if (ranges.empty() || ranges.back().kind != window.kind) {
      ranges.push_back({window.startUs, window.endUs, window.kind});
    } else {
      ranges.back().endUs = window.endUs;
    }
  }
  for (size_t index = 1; index + 1 < ranges.size(); ++index) {
    const int64_t durationUs = ranges[index].endUs - ranges[index].startUs;
    if (ranges[index].kind == SceneKind::Gameplay &&
        durationUs <= 30'000'000 &&
        ranges[index - 1].kind == SceneKind::Cutscene &&
        ranges[index + 1].kind == SceneKind::Cutscene) {
      ranges[index].kind = SceneKind::Cutscene;
    }
  }
  std::vector<ClassifiedRange> compacted;
  compacted.reserve(ranges.size());
  for (const ClassifiedRange& range : ranges) {
    if (!compacted.empty() && compacted.back().kind == range.kind) {
      compacted.back().endUs = range.endUs;
    } else {
      compacted.push_back(range);
    }
  }
  ranges = std::move(compacted);
  for (size_t index = 0; index + 1 < ranges.size(); ++index) {
    // Boundary refinement may move a window edge to a nearby visual cut, but
    // it must not claim temporal precision finer than the classifier's input
    // window. Otherwise two independently refined edges can squeeze a real
    // 12-second observation into a misleading micro-chapter.
    const auto boundaryMargin = [](const ClassifiedRange& range) {
      return std::min<int64_t>(
          kWindowUs, std::max<int64_t>(1, range.endUs - range.startUs - 1));
    };
    const int64_t minimum =
        ranges[index].startUs + boundaryMargin(ranges[index]);
    const int64_t maximum =
        ranges[index + 1].endUs - boundaryMargin(ranges[index + 1]);
    if (maximum < minimum) continue;
    const int64_t nominal = ranges[index].endUs;
    if (const auto refined = strongestBoundaryNear(
            nominal, minimum, maximum, samples)) {
      ranges[index].endUs = *refined;
      ranges[index + 1].startUs = *refined;
    }
  }
  if (!ranges.empty()) {
    ranges.front().startUs = 0;
    ranges.back().endUs = durationUs;
  }
  return ranges;
}

int64_t strongestSplit(int64_t startUs, int64_t endUs,
                       int64_t targetUs,
                       const std::vector<VisualSample>& samples) {
  const int64_t minimum = startUs + kMinimumSplitUs;
  const int64_t maximum = endUs - kMinimumSplitUs;
  if (maximum <= minimum) return std::clamp(targetUs, startUs + 1, endUs - 1);
  const int64_t radius = std::min<int64_t>(45'000'000,
                                          (maximum - minimum) / 2);
  const int64_t searchStart = std::max(minimum, targetUs - radius);
  const int64_t searchEnd = std::min(maximum, targetUs + radius);
  const VisualSample* best = nullptr;
  for (const VisualSample& sample : samples) {
    if (sample.timestampUs < searchStart) continue;
    if (sample.timestampUs > searchEnd) break;
    if (!best || sample.changeScore > best->changeScore) best = &sample;
  }
  return best ? best->timestampUs : std::clamp(targetUs, minimum, maximum);
}

std::vector<ClassifiedRange> splitLongRanges(
    const std::vector<ClassifiedRange>& ranges,
    const std::vector<VisualSample>& samples) {
  std::vector<ClassifiedRange> split;
  for (const ClassifiedRange& range : ranges) {
    const int64_t maximumDuration =
        range.kind == SceneKind::Gameplay ? kMaximumGameplayChapterUs
                                          : kMaximumOtherChapterUs;
    int64_t startUs = range.startUs;
    while (range.endUs - startUs > maximumDuration) {
      const int64_t targetUs =
          startUs + (range.kind == SceneKind::Gameplay
                         ? kTargetGameplayChapterUs
                         : maximumDuration);
      const int64_t boundary =
          strongestSplit(startUs, range.endUs, targetUs, samples);
      if (boundary <= startUs || boundary >= range.endUs) break;
      split.push_back({startUs, boundary, range.kind});
      startUs = boundary;
    }
    if (range.endUs > startUs) {
      split.push_back({startUs, range.endUs, range.kind});
    }
  }
  return split;
}

}  // namespace

float visualChangeScore(const VisualSample& previous,
                        const VisualSample& current) {
  std::array<int, 16> previousHistogram{};
  std::array<int, 16> currentHistogram{};
  double spatialDifference = 0.0;
  for (size_t index = 0; index < kFeatureGridSize; ++index) {
    ++previousHistogram[previous.luma[index] >> 4];
    ++currentHistogram[current.luma[index] >> 4];
    spatialDifference +=
        std::abs(static_cast<int>(previous.luma[index]) -
                 static_cast<int>(current.luma[index]));
  }
  double histogramDifference = 0.0;
  for (size_t index = 0; index < previousHistogram.size(); ++index) {
    histogramDifference +=
        std::abs(previousHistogram[index] - currentHistogram[index]);
  }
  histogramDifference /= static_cast<double>(kFeatureGridSize * 2);
  spatialDifference /= static_cast<double>(kFeatureGridSize * 255);
  return clampUnit(static_cast<float>(histogramDifference * 0.68 +
                                      spatialDifference * 0.32));
}

std::vector<SpeechActivity> buildSpeechActivity(
    const std::vector<playback_video_transcript::Segment>& segments) {
  std::vector<SpeechActivity> activity;
  activity.reserve(segments.size());
  for (const auto& segment : segments) {
    if (segment.endUs <= segment.startUs ||
        playback_video_transcript::isTranscriptSoundAnnotation(
            segment.text)) {
      continue;
    }
    activity.push_back({segment.startUs, segment.endUs});
  }
  std::sort(activity.begin(), activity.end(),
            [](const SpeechActivity& lhs, const SpeechActivity& rhs) {
              if (lhs.startUs != rhs.startUs) return lhs.startUs < rhs.startUs;
              return lhs.endUs < rhs.endUs;
            });
  return activity;
}

std::vector<SceneSuggestion> buildSceneSuggestions(
    int64_t durationUs, const std::vector<VisualSample>& samples,
    const std::vector<SpeechActivity>& speech) {
  if (durationUs <= 0 || samples.empty()) return {};
  const float cutThreshold = adaptiveCutThreshold(samples);
  std::vector<WindowObservation> windows;
  for (int64_t startUs = 0; startUs < durationUs;) {
    const int64_t remaining = durationUs - startUs;
    const int64_t endUs =
        remaining <= kWindowUs ? durationUs : startUs + kWindowUs;
    windows.push_back(
        observeWindow(startUs, endUs, samples, speech, cutThreshold));
    if (endUs == durationUs) break;
    startUs = endUs;
  }
  smoothWindowKinds(&windows);
  std::vector<ClassifiedRange> ranges =
      splitLongRanges(mergeWindows(durationUs, windows, samples), samples);

  std::vector<SceneSuggestion> suggestions;
  suggestions.reserve(ranges.size());
  uint64_t nextId = 1;
  for (const ClassifiedRange& range : ranges) {
    if (range.endUs <= range.startUs) continue;
    SceneSuggestion suggestion;
    suggestion.id = nextId++;
    suggestion.startUs = range.startUs;
    suggestion.endUs = range.endUs;
    suggestion.kind = range.kind;
    suggestion.evidence =
        evidenceForRange(range.startUs, range.endUs, windows);
    suggestion.confidence = confidenceFor(range.kind, suggestion.evidence);
    suggestions.push_back(std::move(suggestion));
  }
  return suggestions;
}

const char* sceneKindLabel(SceneKind kind) {
  switch (kind) {
    case SceneKind::Gameplay:
      return "Gameplay segment";
    case SceneKind::Dialogue:
      return "Dialogue segment";
    case SceneKind::Cutscene:
      return "Cutscene candidate";
    case SceneKind::MenuOrLoading:
      return "Menu/loading segment";
  }
  return "Detected segment";
}

std::string sceneEvidenceSummary(const SceneSuggestion& suggestion) {
  std::array<std::pair<float, const char*>, 4> evidence = {{
      {suggestion.evidence.letterboxRatio, "letterbox"},
      {suggestion.evidence.speechRatio, "dialogue"},
      {suggestion.evidence.darkRatio, "dark frames"},
      {suggestion.evidence.sceneChangeRate, "shot changes"},
  }};
  std::sort(evidence.begin(), evidence.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.first > rhs.first;
            });
  std::string summary;
  for (const auto& item : evidence) {
    if (item.first < 0.04f || summary.size() > 28) continue;
    if (!summary.empty()) summary += " + ";
    summary += item.second;
    if (summary.size() > 18) break;
  }
  return summary.empty() ? "visual continuity" : summary;
}

}  // namespace playback_video_analysis
