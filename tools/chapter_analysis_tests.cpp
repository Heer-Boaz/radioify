#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "core/unicode_display_width.h"
#include "core/utf8.h"
#include "playback/video/analysis/visual_timeline_scan.h"
#include "playback/video/chapter/chapter.h"
#include "playback/video/chapter/evidence_plan.h"
#include "playback/video/chapter/generated_document.h"
#include "playback/video/chapter/integrity.h"
#include "playback/video/chapter/model.h"
#include "playback/video/chapter/presentation.h"
#include "playback/video/chapter/text_evidence.h"
#include "playback/video/subtitle/manager.h"
#include "playback/video/timeline_preview_types.h"

namespace {

bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "chapter_analysis_tests: " << message << '\n';
    return false;
  }
  return true;
}

SubtitleTrack track(std::string label, std::string language, bool forced,
                    bool commentary, std::initializer_list<SubtitleCue> cues) {
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
  ok &= expect(std::strlen(kPlannerAdapterSha256) == 64,
               "release SHA-256 contracts must contain all 64 hex digits");
  ok &= expect(
      sha256Text("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "the native SHA-256 implementation must match a published test vector");
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
          validateAutomaticPartition(60'000'000, singleChapter, &error),
      "homogeneous automatic analysis may publish one undivided chapter");

  const std::vector<Chapter> shortAutomaticChapter = {
      {1, 0, 9'000'000, "Opening", {}},
      {2, 9'000'000, 30'000'000, "Middle", {}},
      {3, 30'000'000, 60'000'000, "Ending", {}}};
  ok &= expect(
      !validateAutomaticPartition(60'000'000, shortAutomaticChapter, &error),
      "automatic chapters shorter than ten seconds must be "
      "rejected");

  constexpr std::int64_t evidenceDurationUs = 300'000'000;
  std::vector<playback_video_analysis::VisualSample> visualTimeline;
  for (std::int64_t timeUs = 0; timeUs < evidenceDurationUs;
       timeUs += 5'000'000) {
    playback_video_analysis::VisualSample sample;
    sample.timestampUs = timeUs;
    sample.meanLuma = 0.5f;
    sample.darkFraction = 0.1f;
    sample.centerEdgeDensity = 0.2f;
    if (timeUs == 100'000'000 || timeUs == 160'000'000) {
      sample.changeScore = 0.8f;
    }
    visualTimeline.push_back(sample);
  }
  const std::vector<ChapterEvidenceInterval> evidencePlan =
      buildChapterEvidencePlan(evidenceDurationUs, visualTimeline);
  bool completeEvidencePartition =
      !evidencePlan.empty() && evidencePlan.size() == 5 &&
      evidencePlan.front().startUs == 0 &&
      evidencePlan.back().endUs == evidenceDurationUs;
  for (std::size_t index = 0; index < evidencePlan.size(); ++index) {
    const ChapterEvidenceInterval &interval = evidencePlan[index];
    completeEvidencePartition =
        completeEvidencePartition &&
        interval.startUs == static_cast<std::int64_t>(index) * 60'000'000 &&
        interval.endUs == static_cast<std::int64_t>(index + 1) * 60'000'000 &&
        interval.sampleTimesUs.size() == 6;
    std::int64_t previousSampleUs = interval.startUs - 1;
    for (const std::int64_t sampleTimeUs : interval.sampleTimesUs) {
      completeEvidencePartition =
          completeEvidencePartition && sampleTimeUs > previousSampleUs &&
          sampleTimeUs >= interval.startUs && sampleTimeUs < interval.endUs;
      previousSampleUs = sampleTimeUs;
    }
  }
  ok &= expect(completeEvidencePartition,
               "chapter evidence must uniformly cover the full timeline as "
               "bounded chronological multi-frame windows");
  const std::vector<ChapterEvidenceInterval> maximumDurationPlan =
      buildChapterEvidencePlan(kMaximumAutomaticChapterVideoDurationUs,
                               visualTimeline);
  ok &= expect(maximumDurationPlan.size() == 60 &&
                   maximumDurationPlan.front().startUs == 0 &&
                   maximumDurationPlan.back().endUs ==
                       kMaximumAutomaticChapterVideoDurationUs,
               "the published sixty-minute support boundary must retain the "
               "ten-second evidence cadence");
  ok &= expect(
      buildChapterEvidencePlan(kMaximumAutomaticChapterVideoDurationUs + 1,
                               visualTimeline)
          .empty(),
      "media beyond the validated duration envelope must be rejected instead "
      "of silently thinning visual evidence");

  playback_video_analysis::VisualTimelineScanRequest scanRequest{
      "video.mp4", 2, evidenceDurationUs, true};
  playback_video_analysis::VisualTimelineScanCheckpoint scanCheckpoint;
  ok &= expect(playback_video_analysis::validVisualTimelineScanCheckpoint(
                   scanRequest, scanCheckpoint),
               "a pristine temporal-scan checkpoint must be resumable");
  scanCheckpoint.initialized = true;
  scanCheckpoint.videoPath = scanRequest.videoPath;
  scanCheckpoint.videoStreamIndex = scanRequest.videoStreamIndex;
  scanCheckpoint.expectedDurationUs = scanRequest.expectedDurationUs;
  scanCheckpoint.requireD3d11 = scanRequest.requireD3d11;
  scanCheckpoint.durationUs = evidenceDurationUs;
  scanCheckpoint.nextSampleUs =
      playback_video_analysis::kVisualSampleIntervalUs;
  scanCheckpoint.samples.push_back(visualTimeline.front());
  ok &= expect(
      playback_video_analysis::validVisualTimelineScanCheckpoint(
          scanRequest, scanCheckpoint),
      "a request-bound checkpoint with complete samples must be resumable");
  scanCheckpoint.nextSampleUs = 0;
  ok &= expect(!playback_video_analysis::validVisualTimelineScanCheckpoint(
                   scanRequest, scanCheckpoint),
               "a checkpoint may not resume before its last published sample");

  std::vector<std::string> captions;
  ok &= expect(
      !generatedFrameCaptionsGrammar(3).empty() &&
          parseGeneratedFrameCaptions(
              R"({"captions":["  A person enters.  ","A door opens.","The room is visible."]})",
              3, &captions, &error) &&
          captions.size() == 3 && captions.front() == "A person enters.",
      "timestamped frame captions must be parsed and normalized in order");
  ok &= expect(
      !parseGeneratedFrameCaptions(R"({"captions":["Only one"],"extra":true})",
                                   2, &captions, &error) &&
          generatedFrameCaptionsGrammar(0).empty() &&
          generatedFrameCaptionsGrammar(7).empty(),
      "caption count and object protocol drift must be rejected");

  std::vector<GeneratedChapterPlanEntry> chapterPlan;
  const std::vector<std::int64_t> chapterEvidenceStarts = {
      0,           60'000'000,  120'000'000, 180'000'000, 240'000'000,
      300'000'000, 360'000'000, 420'000'000, 480'000'000};
  ok &=
      expect(parseChapterLlamaPlan(
                 "00:00:00 - 1. Opening\n00:04:00 - 2. Demonstration\n"
                 "00:08:00 - 3. Result\n",
                 540'000'000, chapterEvidenceStarts, &chapterPlan, &error) &&
                 chapterPlan.size() == 3 && chapterPlan[0].startUs == 0 &&
                 chapterPlan[1].startUs == 240'000'000 &&
                 chapterPlan[2].startUs == 480'000'000,
             "the native Chapter-Llama protocol must retain sampler-owned "
             "timestamps while treating generated labels as non-authoritative");
  ok &= expect(
      !parseChapterLlamaPlan("00:00:00 - Opening\n00:08:00 - Ending\n"
                             "00:04:00 - Middle\n",
                             540'000'000, chapterEvidenceStarts, &chapterPlan,
                             &error) &&
          !parseChapterLlamaPlan("00:01:00 - Late\n", 540'000'000,
                                 chapterEvidenceStarts, &chapterPlan, &error) &&
          !parseChapterLlamaPlan(
              "00:00:00 - Opening\n00:09:00 - Outside video\n", 540'000'000,
              chapterEvidenceStarts, &chapterPlan, &error),
      "native chapter plans must reject reordering, a missing video-start "
      "identity, and timestamps outside the video");
  ok &= expect(
      parseChapterLlamaPlan("00:00:01 - Opening\n00:04:01 - Demonstration\n"
                            "00:07:59 - Result\n",
                            540'000'000, chapterEvidenceStarts, &chapterPlan,
                            &error) &&
          chapterPlan.size() == 3 && chapterPlan[0].startUs == 0 &&
          chapterPlan[1].startUs == 240'000'000 &&
          chapterPlan[2].startUs == 480'000'000,
      "approximate Chapter-Llama times must snap deterministically to the "
      "nearest sampler-owned intervals");

  GeneratedChapterMetadata generatedMetadata;
  ok &= expect(parseGeneratedChapterMetadata(
                   R"({"title":" Arrival ","summary":" A visitor enters. "})",
                   &generatedMetadata, &error) &&
                   generatedMetadata.title == "Arrival" &&
                   generatedMetadata.summary == "A visitor enters.",
               "already-bounded chapter metadata must be normalized as a "
               "typed response");
  ok &= expect(!parseGeneratedChapterMetadata(
                   R"({"title":"Arrival","summary":"Valid","start":10})",
                   &generatedMetadata, &error),
               "bounded chapter metadata must reject model protocol drift");
  std::string generatedOverview;
  ok &= expect(parseGeneratedOverview(
                   R"({"overview":" A demonstration from setup to result. "})",
                   &generatedOverview, &error) &&
                   generatedOverview == "A demonstration from setup to result.",
               "the complete-video overview must be parsed independently");

  GeneratedDocument generated;
  generated.overview = "A demo.";
  generated.chapters = {
      {0, "Opening", "The demo starts."},
      {20'000'000, "Middle", "The work continues."},
      {40'000'000, "Ending", "The demo ends."},
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
  for (GeneratedChapter &chapter : repeatedTitle.chapters) {
    chapter.title = "Gameplay";
  }
  ok &= expect(materializeGeneratedDocument(repeatedTitle, 60'000'000,
                                            {0, 20'000'000, 40'000'000})
                       .status == OperationStatus::Succeeded,
               "a repeated concise title with distinct grounded summaries "
               "must remain valid");
  GeneratedDocument repeatedSummary = generated;
  repeatedSummary.chapters[1].summary = "The demo starts.";
  ok &= expect(materializeGeneratedDocument(repeatedSummary, 60'000'000,
                                            {0, 20'000'000, 40'000'000})
                       .status == OperationStatus::Succeeded,
               "legitimate repeated activities must not be rejected merely "
               "to manufacture unique prose");
  GeneratedDocument truncated = generated;
  truncated.chapters[1].summary = "The generated response stopped";
  ok &= expect(materializeGeneratedDocument(truncated, 60'000'000,
                                            {0, 20'000'000, 40'000'000})
                       .status == OperationStatus::Failed,
               "a grammar-bounded but visibly truncated summary must not be "
               "published");
  GeneratedDocument repeated = generated;
  for (GeneratedChapter &chapter : repeated.chapters) {
    chapter.title = "Same scene";
    chapter.summary = "The same activity.";
  }
  ok &= expect(materializeGeneratedDocument(repeated, 60'000'000,
                                            {0, 20'000'000, 40'000'000})
                       .status == OperationStatus::Failed,
               "constant chapter metadata must be rejected before publication");
  GeneratedDocument duplicate = generated;
  duplicate.chapters[1].startUs = 0;
  ok &= expect(materializeGeneratedDocument(duplicate, 60'000'000,
                                            {0, 20'000'000, 40'000'000})
                       .status == OperationStatus::Failed,
               "duplicate model-selected frame identities must be rejected, "
               "not silently deduplicated");
  ok &= expect(
      !generatedFrameCaptionsGrammar(6).empty() &&
          generatedChapterMetadataGrammar().find("summary") !=
              std::string::npos &&
          !generatedOverviewGrammar().empty(),
      "constrained generation must limit frame captions and already-bounded "
      "metadata prose to their typed protocols");

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
      !navigationTarget(snapshot, 10'000'000, NavigationDirection::Previous) &&
          navigationTarget(snapshot, 25'000'000,
                           NavigationDirection::Previous) == 0 &&
          navigationTarget(snapshot, 25'000'000, NavigationDirection::Next) ==
              40'000'000 &&
          !navigationTarget(snapshot, 50'000'000, NavigationDirection::Next),
      "chapter navigation must move between adjacent sections without an "
      "edge fallback");

  const MarkerProjection markers = projectMarkers(snapshot, 7);
  ok &= expect(markers.boundaryCells == std::vector<int>({2, 4}) &&
                   markers.collisionCells.empty(),
               "chapter boundaries must project proportionally");
  snapshot.chapters = {{1, 0, 10, "A", {}},
                       {2, 10, 11, "B", {}},
                       {3, 11, 12, "C", {}},
                       {4, 12, 1000, "D", {}}};
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
  const auto unsupportedPreview = playback_video_timeline_preview::layoutCells(
      120, 30, 25, 1, 118, 0.5, 1920, 1080, 9.0, 21.0, "00:10",
      previewMetadata(snapshot, 10'000'000));
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
  const OverviewPanelLayout wide = layoutOverviewPanel(snapshot, 120, 30, 25);
  const OverviewPanelLayout narrow = layoutOverviewPanel(snapshot, 60, 20, 15);
  ok &= expect(wide.drawable() && wide.drawer && narrow.drawable() &&
                   !narrow.drawer && wide.width < 120 && narrow.width == 58,
               "overview layout must respond to geometry, not renderer mode");
  ok &= expect(wide.chapterRows.size() == snapshot.chapters.size() &&
                   wide.chapterRows[1].startUs == 20'000'000,
               "each visible chapter row must retain its navigation target");
  ok &= expect(std::find(wide.lines.begin(), wide.lines.end(),
                         "Chapter analysis complete") == wide.lines.end(),
               "a ready overview must present content rather than a redundant "
               "lifecycle status");
  Snapshot verbose = snapshot;
  verbose.overview =
      "A long-form journey crosses luminous forests, city streets, tense "
      "conversations, and major combat encounters before reaching its final "
      "destination.";
  verbose.chapters[1].title =
      "A deliberately long chapter title that must continue on another line";
  const OverviewPanelLayout wrappedOverview =
      layoutOverviewPanel(verbose, 120, 40, 35);
  bool overviewWidthsValid = wrappedOverview.drawable();
  for (const std::string &line : wrappedOverview.lines) {
    overviewWidthsValid = overviewWidthsValid &&
                          utf8DisplayWidth(line) <= wrappedOverview.width - 4;
  }
  const std::string continuationIndent(
      playback_video_timeline_preview::formatTimestamp(
          verbose.chapters[1].startUs)
              .size() +
          2,
      ' ');
  ok &= expect(
      overviewWidthsValid &&
          std::any_of(wrappedOverview.lines.begin(),
                      wrappedOverview.lines.end(),
                      [&](const std::string &line) {
                        return line.size() >= continuationIndent.size() &&
                               line.compare(0, continuationIndent.size(),
                                            continuationIndent) == 0 &&
                               line.find("continue") != std::string::npos;
                      }),
      "overview prose and chapter titles must wrap to the actual drawer width");
  Snapshot manyChapters;
  manyChapters.state = AnalysisState::Ready;
  manyChapters.durationUs = 120'000'000;
  manyChapters.overview =
      "A complete overview whose chapter index remains navigable.";
  for (std::size_t index = 0; index < 12; ++index) {
    manyChapters.chapters.push_back(
        {index + 1, static_cast<std::int64_t>(index) * 10'000'000,
         static_cast<std::int64_t>(index + 1) * 10'000'000,
         "Chapter title " + std::to_string(index + 1), "A grounded summary."});
  }
  const OverviewPanelLayout firstPage =
      layoutOverviewPanel(manyChapters, 100, 16, 14);
  const OverviewPanelLayout lastPage = layoutOverviewPanel(
      manyChapters, 100, 16, 14, firstPage.maximumScrollOffset);
  ok &= expect(
      firstPage.drawable() && firstPage.maximumScrollOffset > 0 &&
          lastPage.scrollOffset == lastPage.maximumScrollOffset &&
          std::any_of(lastPage.lines.begin(), lastPage.lines.end(),
                      [](const std::string &line) {
                        return line.find("Chapter title 12") !=
                               std::string::npos;
                      }),
      "overview scrolling must make the final generated chapter reachable");
  const auto compactPreview = playback_video_timeline_preview::layoutCells(
      50, 10, 6, 1, 48, 0.5, 1920, 1080, 9.0, 21.0, "00:30",
      {"Chapter 2 of 3", "00:20 - 00:40", "A worked example."});
  ok &= expect(compactPreview.drawable() &&
                   compactPreview.metadataPlacement ==
                       playback_video_timeline_preview::CellLayout::
                           MetadataPlacement::BelowImage &&
                   compactPreview.metadataHeight > 0,
               "a short viewport must retain stacked hover metadata");
  const auto wrappedPreview = playback_video_timeline_preview::layoutCells(
      120, 30, 25, 1, 118, 0.5, 1920, 1080, 9.0, 21.0, "00:30",
      {"Chapter 2 of 3 · A very long concrete chapter title that cannot fit "
       "on a single metadata row",
       "00:20 - 00:40",
       "A detailed chapter summary wraps inside the hover surface instead of "
       "being silently clipped at the panel edge."});
  bool previewWidthsValid = wrappedPreview.metadataLines.size() > 3;
  for (const std::string &line : wrappedPreview.metadataLines) {
    previewWidthsValid = previewWidthsValid &&
                         utf8DisplayWidth(line) <= wrappedPreview.metadataWidth;
  }
  ok &= expect(previewWidthsValid,
               "timeline hover metadata must wrap before its row budget is "
               "applied");
  const std::vector<std::string> japaneseWrapped =
      utf8WrapDisplayWidth("東京で始まる章の説明", 8);
  bool japaneseWidthsValid = japaneseWrapped.size() > 1;
  for (const std::string &line : japaneseWrapped) {
    japaneseWidthsValid = japaneseWidthsValid && utf8DisplayWidth(line) <= 8;
  }
  ok &= expect(japaneseWidthsValid,
               "shared wrapping must measure wide Unicode glyphs in terminal "
               "cells for both renderers");
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
  tracks.push_back(track("English", "en", false, false, {intro, body}));

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

} // namespace

int main() {
  const bool ok = runChapterDomainTests() && runTextEvidenceTests();
  return ok ? 0 : 1;
}
