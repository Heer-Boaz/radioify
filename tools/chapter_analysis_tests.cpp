#include "playback/video/chapter/chapter.h"
#include "playback/video/chapter/presentation.h"
#include "playback/video/chapter/text_evidence.h"
#include "playback/video/subtitle/manager.h"
#include "playback/video/timeline_preview_types.h"
#include "core/utf8.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "chapter_analysis_tests: " << message << '\n';
    return false;
  }
  return true;
}

SubtitleTrack track(std::string label, std::string language,
                    bool forced, bool commentary,
                    std::initializer_list<SubtitleCue> cues) {
  SubtitleTrack value;
  value.label = std::move(label);
  value.language = std::move(language);
  value.forced = forced;
  value.commentary = commentary;
  value.cues.assign(cues);
  return value;
}

bool runChapterDomainTests() {
  using namespace playback_video_chapters;
  bool ok = true;
  const std::vector<Chapter> chapters = {
      {1, 0, 20'000'000, "Opening", "The topic is introduced."},
      {2, 20'000'000, 40'000'000, "Demonstration", "A worked example."},
      {3, 40'000'000, 60'000'000, "Conclusion", "The result is reviewed."},
  };
  std::string error;
  ok &= expect(validatePartition(60'000'000, chapters, &error),
               "a complete ordered partition must be accepted");

  std::vector<Chapter> gap = chapters;
  gap[1].startUs += 1;
  ok &= expect(!validatePartition(60'000'000, gap, &error),
               "a chapter gap must be rejected");
  std::vector<Chapter> missingEnd = chapters;
  missingEnd.back().endUs -= 1;
  ok &= expect(!validatePartition(60'000'000, missingEnd, &error),
               "the final chapter must reach the source end");
  std::vector<Chapter> duplicateId = chapters;
  duplicateId.back().id = duplicateId.front().id;
  ok &= expect(!validatePartition(60'000'000, duplicateId, &error),
               "chapter identities must be globally unique");

  Snapshot snapshot;
  snapshot.state = AnalysisState::Ready;
  snapshot.durationUs = 60'000'000;
  snapshot.chapters = chapters;
  ok &= expect(chapterAt(snapshot, 0) == &snapshot.chapters[0] &&
                   chapterAt(snapshot, 20'000'000) == &snapshot.chapters[1] &&
                   chapterAt(snapshot, 59'999'999) == &snapshot.chapters[2],
               "hover lookup must use half-open chapter intervals");

  const MarkerProjection markers = projectMarkers(snapshot, 7);
  ok &= expect(markers.boundaryCells == std::vector<int>({2, 4}) &&
                   markers.collisionCells.empty(),
               "chapter boundaries must project proportionally");
  snapshot.chapters = {
      {1, 0, 10, "A", {}}, {2, 10, 11, "B", {}},
      {3, 11, 12, "C", {}}, {4, 12, 1000, "D", {}}};
  snapshot.durationUs = 1000;
  const MarkerProjection collisions = projectMarkers(snapshot, 8);
  ok &= expect(collisions.boundaryCells == std::vector<int>({0}) &&
                   collisions.collisionCells == std::vector<int>({0}),
               "collapsed boundaries must expose one composite marker");

  snapshot = Snapshot{};
  snapshot.state = AnalysisState::Analyzing;
  snapshot.durationUs = 60'000'000;
  snapshot.progress = 0.37;
  snapshot.phase = "Generating video chapters";
  const std::vector<std::string> progressMetadata =
      previewMetadata(snapshot, 10'000'000);
  ok &= expect(!progressMetadata.empty() &&
                   progressMetadata.front().find("37%") != std::string::npos,
               "analysis progress must project into the hover popover");

  snapshot.state = AnalysisState::Ready;
  snapshot.progress.reset();
  snapshot.overview = "A compact overview of the video.";
  snapshot.chapters = chapters;
  const std::vector<std::string> chapterMetadata =
      previewMetadata(snapshot, 25'000'000);
  ok &= expect(!chapterMetadata.empty() &&
                   chapterMetadata.front().find("Chapter 2 of 3") !=
                       std::string::npos,
               "hover metadata must identify the chapter at the pointer");
  const OverviewPanelLayout wide =
      layoutOverviewPanel(snapshot, 120, 30, 25);
  const OverviewPanelLayout narrow =
      layoutOverviewPanel(snapshot, 60, 20, 15);
  ok &= expect(wide.drawable() && wide.drawer && narrow.drawable() &&
                   !narrow.drawer && wide.width < 120 && narrow.width == 58,
               "overview layout must respond to geometry, not renderer mode");
  const auto compactPreview =
      playback_video_timeline_preview::layoutCells(
          50, 10, 6, 1, 48, 0.5, 1920, 1080, 9.0, 21.0,
          "00:30", {"Chapter 2 of 3", "00:20 - 00:40",
                    "A worked example."});
  ok &= expect(compactPreview.drawable() &&
                   compactPreview.metadataPlacement ==
                       playback_video_timeline_preview::CellLayout::
                           MetadataPlacement::BelowImage &&
                   compactPreview.metadataHeight > 0,
               "a short viewport must retain stacked hover metadata");
  return ok;
}

bool runTextEvidenceTests() {
  using namespace playback_video_chapters;
  SubtitleCue intro;
  intro.startUs = 0;
  intro.endUs = 2'000'000;
  intro.text = "Welcome";
  SubtitleCue body;
  body.startUs = 2'000'000;
  body.endUs = 8'000'000;
  body.text = "This is the main dialogue";
  std::vector<SubtitleTrack> tracks;
  tracks.push_back(track("Japanese", "ja", false, false, {intro, body}));
  tracks.push_back(track("English forced", "en", true, false, {intro}));
  tracks.push_back(
      track("English commentary", "en", false, true, {intro, body}));
  tracks.push_back(
      track("English", "en", false, false, {intro, body}));

  const auto evidence = selectEnglishTextEvidence(tracks, "movie.mkv");
  bool ok = true;
  ok &= expect(evidence && evidence->label == "English" &&
                   evidence->language == "en" && evidence->cues.size() == 2,
               "full English dialogue must beat forced and commentary tracks");
  if (evidence) {
    ok &= expect(textNear(*evidence, 3'000'000, 2'000'000, 64) ==
                     "Welcome This is the main dialogue",
                 "VLM text evidence must retain bounded nearby dialogue");
    ok &= expect(textNear(*evidence, 3'000'000, 2'000'000, 10).size() == 10,
                 "prompt evidence must obey its byte budget");
  }

  TextEvidence unicodeEvidence;
  unicodeEvidence.cues.push_back({0, 1'000'000, "Tokyo 東京"});
  const std::string unicodePrefix =
      textNear(unicodeEvidence, 500'000, 1'000'000, 8);
  ok &= expect(unicodePrefix.size() <= 8 && isValidUtf8(unicodePrefix),
               "prompt byte limits must not split a UTF-8 code point");

  tracks.erase(tracks.begin() + 3);
  const auto fallback = selectEnglishTextEvidence(tracks, "movie.mkv");
  ok &= expect(fallback && fallback->label == "English forced",
               "a forced English track must remain a fallback to commentary");
  return ok;
}

}  // namespace

int main() {
  const bool ok = runChapterDomainTests() && runTextEvidenceTests();
  return ok ? 0 : 1;
}
