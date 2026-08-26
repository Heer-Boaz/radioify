#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "playback/video/transcript/cue.h"

namespace playback_video_analysis {

inline constexpr int kFeatureGridColumns = 32;
inline constexpr int kFeatureGridRows = 18;
inline constexpr size_t kFeatureGridSize =
    static_cast<size_t>(kFeatureGridColumns * kFeatureGridRows);
inline constexpr int64_t kVisualSampleIntervalUs = 500'000;

struct VisualSample {
  int64_t timestampUs = 0;
  std::array<uint8_t, kFeatureGridSize> luma{};
  float meanLuma = 0.0f;
  float darkFraction = 0.0f;
  float letterboxConfidence = 0.0f;
  float borderEdgeDensity = 0.0f;
  float centerEdgeDensity = 0.0f;
  float changeScore = 0.0f;
};

struct SpeechActivity {
  int64_t startUs = 0;
  int64_t endUs = 0;
};

enum class SceneKind : uint8_t {
  Gameplay,
  Dialogue,
  Cutscene,
  MenuOrLoading,
};

struct SceneEvidence {
  float speechRatio = 0.0f;
  float letterboxRatio = 0.0f;
  float darkRatio = 0.0f;
  float sceneChangeRate = 0.0f;
  float visualMotion = 0.0f;
  float hudLikelihood = 0.0f;
};

struct SceneSuggestion {
  uint64_t id = 0;
  int64_t startUs = 0;
  int64_t endUs = 0;
  SceneKind kind = SceneKind::Gameplay;
  float confidence = 0.0f;
  SceneEvidence evidence;
};

// Measures an adaptive scene-cut signal from two normalized luma grids.
// Histogram distance keeps ordinary camera movement from looking like a hard
// cut, while the spatial term still catches similarly lit shot changes.
float visualChangeScore(const VisualSample& previous,
                        const VisualSample& current);

// Converts canonical indexed-transcript cue intervals into speech evidence.
// Cue timing remains owned by transcript production; bracketed sound/music
// annotations do not count as dialogue.
std::vector<SpeechActivity> buildSpeechActivity(
    const std::vector<playback_video_transcript::Segment>& segments);

// Pure, deterministic grouping/classification. Suggestions cover the source
// timeline, but remain descriptive ranges: callers decide whether selecting a
// range should become an edit operation.
std::vector<SceneSuggestion> buildSceneSuggestions(
    int64_t durationUs, const std::vector<VisualSample>& samples,
    const std::vector<SpeechActivity>& speech);

const char* sceneKindLabel(SceneKind kind);
std::string sceneEvidenceSummary(const SceneSuggestion& suggestion);

}  // namespace playback_video_analysis
