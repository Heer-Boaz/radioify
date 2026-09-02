#include <iostream>
#include <string>
#include <vector>

#include "core/utf8.h"
#include "playback/video/chapter/chapter.h"
#include "playback/video/chapter/generated_document.h"
#include "playback/video/chapter/presentation.h"
#include "playback/video/chapter/text_evidence.h"
#include "playback/video/subtitle/manager.h"
#include "playback/video/timeline_preview_types.h"

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
  ok &= expect(validateAutomaticPartition(60'000'000, chapters, &error),
               "three sufficiently long chapters must satisfy automatic "
               "chapter policy");
  ok &= expect(validateAutomaticAnalysis(
                   60'000'000, "The video moves through three sections.",
                   chapters, &error),
               "a complete bounded analysis must be publishable");
  std::vector<Chapter> multilineMetadata = chapters;
  multilineMetadata[1].summary = "A line.\nInjected layout text.";
  ok &= expect(!validateAutomaticAnalysis(
                   60'000'000, "The video moves through three sections.",
                   multilineMetadata, &error),
               "persisted model metadata must not inject multiline UI text");

  const std::vector<Chapter> singleChapter = {
      {1, 0, 60'000'000, "Entire video", "One undivided section."}};
  ok &= expect(
      validatePartition(60'000'000, singleChapter, &error) &&
          !validateAutomaticPartition(60'000'000, singleChapter, &error),
      "a structural partition must remain distinct from automatic marker "
      "policy");

  const std::vector<Chapter> shortAutomaticChapter = {
      {1, 0, 9'000'000, "Opening", {}},
      {2, 9'000'000, 30'000'000, "Middle", {}},
      {3, 30'000'000, 60'000'000, "Ending", {}}};
  ok &= expect(
      !validateAutomaticPartition(60'000'000, shortAutomaticChapter, &error),
      "automatic chapters shorter than ten seconds must be "
      "rejected");

  ok &= expect(automaticChapterSampleTimes(29'999'999).empty(),
               "videos below the minimum chapterable duration must not be "
               "sampled");
  ok &= expect(automaticChapterSampleTimes(30'000'000) ==
                   std::vector<std::int64_t>({0, 10'000'000, 20'000'000}),
               "the minimum-duration source must expose exactly three valid "
               "chapter starts");
  const std::int64_t sampledDuration = 192'928'511;
  const std::vector<std::int64_t> sampleTimes =
      automaticChapterSampleTimes(sampledDuration);
  bool validSpacing = sampleTimes.size() == kMaximumAutomaticChapterCount &&
                      sampleTimes.front() == 0;
  for (std::size_t index = 1; index < sampleTimes.size(); ++index) {
    validSpacing =
        validSpacing && sampleTimes[index] - sampleTimes[index - 1] >=
                            kMinimumAutomaticChapterDurationUs;
  }
  validSpacing = validSpacing && sampledDuration - sampleTimes.back() >=
                                     kMinimumAutomaticChapterDurationUs;
  ok &= expect(validSpacing,
               "every sampled frame identity must be a valid chapter start");

  std::string observation;
  ok &= expect(parseGeneratedObservation(
                   R"({"observation":"  A person enters the store.  "})",
                   &observation, &error) &&
                   observation == "A person enters the store.",
               "frame observations must be parsed and normalized in "
               "isolation");
  ok &= expect(
      !parseGeneratedObservation(R"({"observation":"Valid","extra":true})",
                                 &observation, &error),
      "an observation with protocol drift must be rejected");

  GeneratedSegmentationPlan plan;
  ok &= expect(
      parseGeneratedSegmentationPlan(
          R"({"progression":"Arrival, demonstration, and conclusion.","chapter_count":3})",
          6, &plan, &error) &&
          plan.chapterCount == 3,
      "global timeline reasoning must precede the bounded chapter "
      "count");
  ok &= expect(!parseGeneratedSegmentationPlan(
                   R"({"progression":"Too many sections.","chapter_count":7})",
                   6, &plan, &error),
               "the generated chapter count must remain inside sampled "
               "evidence");
  GeneratedChangePointScore changePointScore;
  ok &= expect(parseGeneratedChangePointScore(R"({"score":85})",
                                              &changePointScore, &error) &&
                   changePointScore.score == 85,
               "each candidate gap must produce a typed confidence score");
  ok &= expect(!parseGeneratedChangePointScore(R"({"score":"high"})",
                                               &changePointScore, &error),
               "change-point protocol drift must be rejected");
  std::vector<GeneratedChangePointScore> scores = {
      {10}, {90}, {20}, {80}, {30}};
  std::vector<std::size_t> selectedFrames;
  ok &= expect(selectGeneratedBoundaries(scores, 3, &selectedFrames, &error) &&
                   selectedFrames == std::vector<std::size_t>({1, 3, 5}),
               "the strongest scored gaps must map to chronological sampled "
               "frame identities");
  scores[4].score = 80;
  ok &= expect(selectGeneratedBoundaries(scores, 3, &selectedFrames, &error) &&
                   selectedFrames == std::vector<std::size_t>({1, 3, 5}),
               "equal semantic scores must use balanced timeline coverage "
               "instead of chronological bias");
  const std::vector<GeneratedChangePointScore> noEvidence(5);
  ok &=
      expect(!selectGeneratedBoundaries(noEvidence, 3, &selectedFrames, &error),
             "a plan cannot manufacture boundaries without semantic "
             "evidence");

  GeneratedChapterMetadata metadata;
  ok &= expect(
      parseGeneratedChapterMetadata(
          R"({"title":"Store Arrival","summary":"A visitor enters the shop."})",
          &metadata, &error) &&
          metadata.title == "Store Arrival" &&
          metadata.summary == "A visitor enters the shop.",
      "chapter metadata must be parsed independently from boundary "
      "selection");
  ok &= expect(
      parseGeneratedChapterMetadata(
          R"({"title":"Arrival","summary":"A visitor enters the shop."})",
          &metadata, &error) &&
          metadata.title == "Arrival",
      "a concise one-word chapter title must be valid");
  std::string overview;
  ok &= expect(
      parseGeneratedOverview(
          R"({"overview":"A demonstration moves from setup to result."})",
          &overview, &error) &&
          overview == "A demonstration moves from setup to result.",
      "the overview must be parsed as a separate completed stage");

  GeneratedDocument generated;
  generated.overview = "A demo.";
  generated.chapters = {
      {1, {"Opening", "The demo starts."}},
      {2, {"Middle", "The work continues."}},
      {3, {"Ending", "The demo ends."}},
  };
  const AnalysisResult parsed = materializeGeneratedDocument(
      generated, 60'000'000, {0, 20'000'000, 40'000'000});
  ok &=
      expect(parsed.status == OperationStatus::Succeeded &&
                 parsed.overview == "A demo." && parsed.chapters.size() == 3 &&
                 parsed.chapters[1].startUs == 20'000'000 &&
                 parsed.chapters.back().endUs == 60'000'000,
             "generated frame identities must map to authoritative sample "
             "timestamps");
  GeneratedDocument repeatedTitle = generated;
  for (GeneratedChapter& chapter : repeatedTitle.chapters) {
    chapter.metadata.title = "Gameplay";
  }
  ok &= expect(materializeGeneratedDocument(repeatedTitle, 60'000'000,
                                            {0, 20'000'000, 40'000'000})
                       .status == OperationStatus::Succeeded,
               "a repeated concise title with distinct grounded summaries "
               "must remain valid");
  GeneratedDocument repeated = generated;
  for (GeneratedChapter& chapter : repeated.chapters) {
    chapter.metadata.title = "Same scene";
    chapter.metadata.summary = "The same activity.";
  }
  ok &= expect(materializeGeneratedDocument(repeated, 60'000'000,
                                            {0, 20'000'000, 40'000'000})
                       .status == OperationStatus::Failed,
               "constant chapter metadata must be rejected before publication");
  GeneratedDocument duplicate = generated;
  duplicate.chapters[1].startFrame = 1;
  ok &= expect(materializeGeneratedDocument(duplicate, 60'000'000,
                                            {0, 20'000'000, 40'000'000})
                       .status == OperationStatus::Failed,
               "duplicate model-selected frame identities must be rejected, "
               "not silently deduplicated");
  const std::string planGrammar = generatedSegmentationPlanGrammar(6);
  const std::string changePointGrammar = generatedChangePointScoreGrammar();
  ok &= expect(
      !planGrammar.empty() &&
          planGrammar.find("chapter_count") != std::string::npos &&
          !changePointGrammar.empty() &&
          changePointGrammar.find("score") != std::string::npos &&
          changePointGrammar.find("start_seconds") == std::string::npos &&
          !generatedObservationGrammar().empty() &&
          !generatedChapterMetadataGrammar().empty() &&
          !generatedOverviewGrammar().empty(),
      "the constrained grammar must enumerate only valid sampled "
      "frame identities and keep every inference stage typed");

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
  ok &= expect(
      !navigationTarget(snapshot, 10'000'000,
                        NavigationDirection::Previous) &&
          navigationTarget(snapshot, 25'000'000,
                           NavigationDirection::Previous) == 0 &&
          navigationTarget(snapshot, 25'000'000,
                           NavigationDirection::Next) == 40'000'000 &&
          !navigationTarget(snapshot, 50'000'000,
                            NavigationDirection::Next),
      "chapter navigation must move between adjacent sections without an "
      "edge fallback");

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
  ok &= expect(progressMetadata.empty() &&
                   !layoutOverviewPanel(snapshot, 120, 30, 25).drawable(),
               "in-progress analysis must not create chapter hover metadata "
               "or an empty overview surface");

  snapshot.state = AnalysisState::Unsupported;
  snapshot.progress.reset();
  snapshot.phase.clear();
  snapshot.detail = "D3D11 hardware decoding is unavailable.";
  ok &= expect(previewMetadata(snapshot, 10'000'000).empty(),
               "unsupported analysis must preserve the frame-only hover "
               "preview without an empty metadata panel");
  ok &= expect(!layoutOverviewPanel(snapshot, 120, 30, 25).drawable(),
               "unsupported analysis must not create an overview surface");
  const auto unsupportedPreview =
      playback_video_timeline_preview::layoutCells(
          120, 30, 25, 1, 118, 0.5, 1920, 1080, 9.0, 21.0,
          "00:10", previewMetadata(snapshot, 10'000'000));
  ok &= expect(
      unsupportedPreview.drawable() &&
          unsupportedPreview.metadataPlacement ==
              playback_video_timeline_preview::CellLayout::MetadataPlacement::
                  None &&
          unsupportedPreview.metadataWidth == 0 &&
          unsupportedPreview.metadataHeight == 0,
      "unsupported analysis must not reserve hover layout space for text");

  snapshot.state = AnalysisState::Ready;
  snapshot.detail.clear();
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
    ok &= expect(textInInterval(*evidence, 0, 2'000'000, 64) == "Welcome" &&
                     textInInterval(*evidence, 2'000'000, 10'000'000, 64) ==
                         "This is the main dialogue",
                 "adjacent sampled intervals must own disjoint subtitle "
                 "evidence");
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
