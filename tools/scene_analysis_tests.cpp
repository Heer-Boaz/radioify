#include "playback/video/analysis/scene_analysis.h"
#include "playback/video/edit/scene_suggestions.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "scene_analysis_tests: " << message << '\n';
  return false;
}

playback_video_analysis::VisualSample sampleAt(int64_t timestampUs) {
  playback_video_analysis::VisualSample sample;
  sample.timestampUs = timestampUs;
  sample.meanLuma = 0.46f;
  sample.darkFraction = 0.04f;
  sample.borderEdgeDensity = 0.20f;
  sample.centerEdgeDensity = 0.22f;
  sample.changeScore = 0.025f;
  for (size_t index = 0; index < sample.luma.size(); ++index) {
    sample.luma[index] = static_cast<uint8_t>(80u + index % 100u);
  }
  return sample;
}

}  // namespace

int main() {
  namespace analysis = playback_video_analysis;
  bool ok = true;

  analysis::VisualSample black;
  analysis::VisualSample white;
  white.luma.fill(255);
  ok &= expect(analysis::visualChangeScore(black, black) == 0.0f &&
                   analysis::visualChangeScore(black, white) > 0.95f,
               "visual change scoring must separate identical and opposite frames");

  using playback_video_edit::SceneSuggestionFilter;
  using playback_video_edit::SceneSuggestionKind;
  ok &= expect(
      playback_video_edit::sceneSuggestionMatchesFilter(
          SceneSuggestionKind::Cutscene, SceneSuggestionFilter::All) &&
          playback_video_edit::sceneSuggestionMatchesFilter(
              SceneSuggestionKind::Cutscene,
              SceneSuggestionFilter::Cutscenes) &&
          !playback_video_edit::sceneSuggestionMatchesFilter(
              SceneSuggestionKind::Dialogue,
              SceneSuggestionFilter::Cutscenes),
      "suggestion filters must retain only their named segment kind");
  SceneSuggestionFilter cycledFilter = SceneSuggestionFilter::All;
  for (int step = 0; step < 5; ++step) {
    cycledFilter =
        playback_video_edit::nextSceneSuggestionFilter(cycledFilter);
  }
  ok &= expect(
      cycledFilter == SceneSuggestionFilter::All &&
          std::string(playback_video_edit::sceneSuggestionKindLabel(
              SceneSuggestionKind::Gameplay)) == "Gameplay segment" &&
          std::string(playback_video_edit::sceneSuggestionStrengthLabel(
              0.71f)) == "Possible" &&
          std::string(playback_video_edit::sceneSuggestionStrengthLabel(
              0.72f)) == "Strong",
      "suggestion filters, kind names, and confidence bands must remain "
      "stable presentation policy");

  const std::vector<playback_video_transcript::Segment> transcript = {
      {0, 10'000'000, "[music]"},
      {10'000'000, 40'000'000, "This cue has a needlessly long display span"},
      {50'000'000, 52'000'000, "Short line"},
      {53'000'000, 55'000'000, "(Spoken translation)"},
  };
  const std::vector<analysis::SpeechActivity> speech =
      analysis::buildSpeechActivity(transcript);
  ok &= expect(speech.size() == 3,
               "sound annotations must not count as dialogue, while spoken "
               "parenthetical text must remain evidence");
  ok &= expect(speech.size() >= 1 &&
                   speech[0].startUs == transcript[1].startUs &&
                   speech[0].endUs == transcript[1].endUs,
               "transcript cue timing must remain the single speech owner");
  ok &= expect(speech.size() >= 2 && speech[1].startUs == 50'000'000 &&
                   speech[1].endUs == 52'000'000,
               "already concise speech timing must be retained");
  ok &= expect(speech.size() >= 3 && speech[2].startUs == 53'000'000 &&
                   speech[2].endUs == 55'000'000,
               "parenthetical spoken text must remain dialogue evidence");

  constexpr int64_t durationUs = 72'000'000;
  std::vector<analysis::VisualSample> samples;
  for (int64_t timestampUs = 0; timestampUs < durationUs;
       timestampUs += 500'000) {
    analysis::VisualSample sample = sampleAt(timestampUs);
    if (timestampUs >= 24'000'000 && timestampUs < 48'000'000) {
      sample.letterboxConfidence = 0.92f;
      sample.borderEdgeDensity = 0.08f;
    } else if (timestampUs >= 48'000'000 &&
               timestampUs < 60'000'000) {
      sample.meanLuma = 0.03f;
      sample.darkFraction = 0.92f;
      sample.borderEdgeDensity = 0.01f;
      sample.centerEdgeDensity = 0.01f;
    }
    if (timestampUs == 24'000'000 || timestampUs == 48'000'000 ||
        timestampUs == 60'000'000) {
      sample.changeScore = 0.80f;
    }
    samples.push_back(std::move(sample));
  }
  const std::vector<analysis::SpeechActivity> sceneSpeech = {
      {26'000'000, 30'000'000},
      {34'000'000, 38'000'000},
      {41'000'000, 45'000'000},
  };
  const std::vector<analysis::SceneSuggestion> suggestions =
      analysis::buildSceneSuggestions(durationUs, samples, sceneSpeech);
  ok &= expect(!suggestions.empty() && suggestions.front().startUs == 0 &&
                   suggestions.back().endUs == durationUs,
               "scene suggestions must cover the complete source timeline");
  ok &= expect(std::adjacent_find(
                   suggestions.begin(), suggestions.end(),
                   [](const auto& lhs, const auto& rhs) {
                     return lhs.endUs != rhs.startUs ||
                            lhs.endUs <= lhs.startUs;
                   }) == suggestions.end(),
               "scene suggestions must be ordered, contiguous ranges");
  const auto cutscene = std::find_if(
      suggestions.begin(), suggestions.end(), [](const auto& suggestion) {
        return suggestion.kind == analysis::SceneKind::Cutscene;
      });
  const auto loading = std::find_if(
      suggestions.begin(), suggestions.end(), [](const auto& suggestion) {
        return suggestion.kind == analysis::SceneKind::MenuOrLoading;
      });
  ok &= expect(cutscene != suggestions.end() &&
                   cutscene->startUs >= 20'000'000 &&
                   cutscene->endUs <= 52'000'000,
               "letterboxed dialogue windows must form a cutscene candidate");
  ok &= expect(loading != suggestions.end() &&
                   loading->startUs >= 44'000'000 &&
                   loading->endUs <= 64'000'000,
               "dark static windows must form a loading/menu candidate");

  if (cutscene != suggestions.end()) {
    playback_video_edit::Timeline timeline(durationUs);
    ok &= expect(timeline.rippleDelete({30'000'000, 36'000'000}),
                 "projection fixture must create an edited source gap");
    const playback_video_edit::SceneSuggestionSnapshot projected =
        playback_video_edit::projectSceneSuggestion(*cutscene, timeline, true);
    ok &= expect(projected.selected && projected.spans.size() == 2,
                 "one source suggestion must project across retained EDL clips");
    ok &= expect(
        playback_video_edit::sceneSuggestionAtTimeline(
            {projected}, projected.spans.front().timelineStartUs, 0) ==
            projected.id,
        "timeline hit-testing must preserve semantic suggestion identity");
    playback_video_edit::SceneSuggestionSnapshot precedingSuggestion =
        projected;
    precedingSuggestion.spans = {projected.spans.front()};
    playback_video_edit::SceneSuggestionSnapshot adjacentSuggestion;
    adjacentSuggestion.id = projected.id + 1;
    adjacentSuggestion.spans.push_back(
        {projected.spans.front().timelineEndUs,
         projected.spans.front().timelineEndUs + 1'000'000});
    ok &= expect(
        playback_video_edit::sceneSuggestionAtTimeline(
            {precedingSuggestion, adjacentSuggestion},
            projected.spans.front().timelineEndUs, 0) ==
            adjacentSuggestion.id,
        "a shared half-open boundary must select the suggestion that starts "
        "there instead of the preceding suggestion's exclusive end");
    playback_video_edit::Timeline removedTimeline(durationUs);
    ok &= expect(removedTimeline.rippleDelete(
                     {cutscene->startUs, cutscene->endUs}) &&
                     !playback_video_edit::sceneSuggestionVisibleOnTimeline(
                         *cutscene, removedTimeline) &&
                     playback_video_edit::projectSceneSuggestion(
                         *cutscene, removedTimeline, true)
                         .spans.empty(),
                 "fully removed suggestions must leave navigation and the "
                 "timeline projection together");
  }

  constexpr int64_t squeezedDurationUs = 36'000'000;
  std::vector<analysis::VisualSample> squeezedSamples;
  for (int64_t timestampUs = 0; timestampUs < squeezedDurationUs;
       timestampUs += analysis::kVisualSampleIntervalUs) {
    analysis::VisualSample sample = sampleAt(timestampUs);
    if (timestampUs >= 12'000'000 && timestampUs < 24'000'000) {
      sample.letterboxConfidence = 0.95f;
      sample.borderEdgeDensity = 0.03f;
    }
    if (timestampUs == 15'500'000 || timestampUs == 20'500'000) {
      sample.changeScore = 0.98f;
    }
    squeezedSamples.push_back(std::move(sample));
  }
  const std::vector<analysis::SceneSuggestion> squeezedSuggestions =
      analysis::buildSceneSuggestions(
          squeezedDurationUs, squeezedSamples,
          {{24'000'000, squeezedDurationUs}});
  const auto squeezedCutscene = std::find_if(
      squeezedSuggestions.begin(), squeezedSuggestions.end(),
      [](const auto& suggestion) {
        return suggestion.kind == analysis::SceneKind::Cutscene;
      });
  ok &= expect(
      squeezedCutscene != squeezedSuggestions.end() &&
          squeezedCutscene->endUs - squeezedCutscene->startUs >=
              12'000'000,
      "independent visual-boundary refinements must not squeeze a classified "
      "window below the model's temporal resolution");

  if (!ok) return 1;
  std::cout << "scene_analysis_tests: PASS\n";
  return 0;
}
