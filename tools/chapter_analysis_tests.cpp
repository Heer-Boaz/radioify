#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "core/file_output.h"
#include "core/unicode_display_width.h"
#include "core/utf8.h"
#include "playback/video/analysis/visual_timeline_scan.h"
#include "playback/video/chapter/chapter.h"
#include "playback/video/chapter/cache.h"
#include "playback/video/chapter/evidence_plan.h"
#include "playback/video/chapter/generated_document.h"
#include "playback/video/chapter/integrity.h"
#include "playback/video/chapter/model.h"
#include "playback/video/chapter/presentation.h"
#include "playback/video/chapter/prompt_contract.h"
#include "playback/video/chapter/text_evidence.h"
#include "playback/video/subtitle/manager.h"
#include "playback/video/timeline_preview_types.h"
#include "playback/video/transcript/artifact.h"
#include "playback/video/transcript/document.h"
#include "playback/video/transcript/provenance.h"

namespace {

bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "chapter_analysis_tests: " << message << '\n';
    return false;
  }
  return true;
}

std::string panelLineText(
    const playback_video_chapters::OverviewPanelLayout::Line &line) {
  std::string text;
  int column = 0;
  for (const auto &run : line.runs) {
    if (run.column > column)
      text.append(static_cast<std::size_t>(run.column - column), ' ');
    text += run.text;
    column = run.column + utf8DisplayWidth(run.text);
  }
  return text;
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
  ok &= expect(std::strlen(kSpeechPlanAdapterSha256) == 64,
               "release SHA-256 contracts must contain all 64 hex digits");
  ok &= expect(std::strlen(kChapterPlanAdapterSha256) == 64,
               "both published adapter contracts must contain all 64 hex digits");
  ok &= expect(
      sha256Text("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "the native SHA-256 implementation must match a published test vector");
  const std::vector<Chapter> chapters = {
      {1, 0, 20'000'000, "Opening"},
      {2, 20'000'000, 40'000'000, "Demonstration"},
      {3, 40'000'000, 60'000'000, "Conclusion"},
  };
  std::string error;
  ok &= expect(validatePartition(60'000'000, chapters, &error),
               "a complete ordered partition must be accepted");
  ok &= expect(validateAutomaticPartition(60'000'000, chapters, &error),
               "three sufficiently long chapters must satisfy automatic "
               "chapter policy");
  ok &= expect(validateAutomaticAnalysis(60'000'000, chapters, &error),
                "a complete bounded analysis must be publishable");
  std::vector<Chapter> multilineMetadata = chapters;
  multilineMetadata[1].title = "A line.\nInjected layout text.";
  ok &= expect(!validateAutomaticAnalysis(60'000'000, multilineMetadata,
                                          &error),
                "persisted model metadata must not inject multiline UI text");

  const std::vector<Chapter> singleChapter = {
      {1, 0, 60'000'000, "Entire video"}};
  ok &= expect(
      validatePartition(60'000'000, singleChapter, &error) &&
          validateAutomaticPartition(60'000'000, singleChapter, &error),
      "homogeneous automatic analysis may publish one undivided chapter");

  const std::vector<Chapter> shortAutomaticChapter = {
      {1, 0, 9'000'000, "Opening"},
      {2, 9'000'000, 30'000'000, "Middle"},
      {3, 30'000'000, 60'000'000, "Ending"}};
  ok &= expect(
      validateAutomaticPartition(60'000'000, shortAutomaticChapter, &error),
      "automatic chapter validation must not invent a minimum chapter "
      "duration");

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
      buildSpeechGuidedChapterEvidencePlan(evidenceDurationUs,
                                           {0, 100'000'000, 160'000'000});
  ok &= expect(
      evidencePlan.size() == 3 && evidencePlan.front().startUs == 0 &&
          evidencePlan.back().endUs == evidenceDurationUs &&
          evidencePlan[0].sampleTimesUs ==
              std::vector<std::int64_t>{1'000'000} &&
          evidencePlan[1].sampleTimesUs ==
              std::vector<std::int64_t>{100'000'000} &&
          evidencePlan[2].sampleTimesUs ==
              std::vector<std::int64_t>{160'000'000} &&
          evidencePlan[0].endUs == 50'500'000 &&
          evidencePlan[1].startUs == 50'500'000 &&
          evidencePlan[1].endUs == 130'000'000 &&
          evidencePlan[2].startUs == 130'000'000,
      "speech-guided evidence must sample only ASR-predicted boundaries and "
      "avoid the black opening frame");
  const std::vector<ChapterEvidenceInterval> sparseEvidencePlan =
      buildSpeechGuidedChapterEvidencePlan(evidenceDurationUs,
                                           {0, 299'000'000});
  ok &= expect(
      sparseEvidencePlan.size() == 2 &&
          sparseEvidencePlan[0].sampleTimesUs ==
              std::vector<std::int64_t>{1'000'000} &&
          sparseEvidencePlan[1].sampleTimesUs ==
              std::vector<std::int64_t>{299'000'000},
      "large gaps between ASR predictions must never create periodic fallback "
      "samples");
  ok &= expect(
      buildSpeechGuidedChapterEvidencePlan(evidenceDurationUs,
                                           {10'000'000, 100'000'000})
              .empty() &&
          buildSpeechGuidedChapterEvidencePlan(evidenceDurationUs,
                                               {0, 100'000'000, 100'000'000})
              .empty(),
      "speech-guided evidence must reject incomplete or duplicate boundary "
      "plans instead of inventing periodic samples");

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

  std::string observation;
  ok &= expect(
      normalizeGeneratedFrameCaption("  A person enters as a door opens.  ",
                                     &observation, &error) &&
          observation == "A person enters as a door opens.",
      "unconstrained upstream-style frame captions must be normalized");
  ok &= expect(!normalizeGeneratedFrameCaption({}, &observation, &error),
               "empty frame captions must be rejected");

  const auto captionerPrompt = buildChapterLlamaMiniCpmV2CaptionPrompt(
      "(<image>./</image>)", 1024);
  ok &= expect(
      captionerPrompt &&
          *captionerPrompt ==
              "<user>(<image>./</image>)\nWhat is the content of this "
              "image?<AI>",
      "the vision captioner prompt must match HwwwH/MiniCPM-V-2's "
      "published Chapter-Llama extraction turn byte for byte");

  const std::vector<ChapterPromptEvidence> asrPromptEvidence = {
      {0, "Opening narration."}, {5'000'000, "The subject changes."}};
  const std::vector<ChapterPromptEvidence> captionPromptEvidence = {
      {0, "A title card."}, {3'000'000, "A person enters."}};
  const auto speechPrompt = buildChapterLlamaSpeechPrompt(
      60'000'000, asrPromptEvidence, 16 * 1024);
  const auto combinedPrompt = buildChapterLlamaCaptionAsrPrompt(
      60'000'000, captionPromptEvidence, asrPromptEvidence, 16 * 1024);
  ok &= expect(
      speechPrompt &&
          *speechPrompt ==
              "Given the complete transcript of a video of duration 00:01:00, "
              "segment the text into distinct chapters based on thematic "
              "shifts or changes in topics.\nIdentify the approximate start "
              "time of each chapter in the format 'hh:mm:ss - Title'. Ensure "
              "each chapter entry is on a new line. Focus on significant topic "
              "changes that would merit a new chapter in a video, but do not "
              "provide summaries of the chapters.\nHere is the transcript to "
              "analyze:\n00:00:00: Opening narration.\n00:00:05: The subject "
              "changes.\n",
      "the speech selector prompt must match PromptASR byte for byte");
  ok &= expect(
      combinedPrompt &&
          *combinedPrompt ==
              "Given the complete transcript of a video of duration 00:01:00, "
              "use the provided captions and ASR transcript to identify "
              "distinct chapters based on content shifts.\nIdentify the "
              "approximate start time of each chapter in the format 'hh:mm:ss "
              "- Title'. Ensure each chapter entry is on a new line. Focus on "
              "significant topic changes that would merit a new chapter in a "
              "video, but do not provide summaries of the chapters.\nHere is "
              "the transcript to analyze:\nASR 00:00:00: Opening narration.\n"
              "Caption 00:00:00: A title card.\nCaption 00:00:03: A person "
              "enters.\nASR 00:00:05: The subject changes.",
      "the captions-plus-ASR protocol must retain reference stable ordering "
      "and omit an invented trailing record delimiter");

  std::vector<GeneratedChapterPlanEntry> chapterPlan;
  ok &= expect(parseChapterLlamaPlan(
                   "00:00:00 - Opening\n00:04:00 - Demonstration\n"
                   "00:08:00 - Result\n",
                   540'000'000, &chapterPlan, &error) &&
                   chapterPlan.size() == 3 && chapterPlan[0].startUs == 0 &&
                   chapterPlan[0].title == "Opening" &&
                   chapterPlan[1].startUs == 240'000'000 &&
                   chapterPlan[1].title == "Demonstration" &&
                   chapterPlan[2].startUs == 480'000'000,
               "the native Chapter-Llama protocol must retain its joint title "
               "decision at its native timestamps");
  ok &= expect(
      !parseChapterLlamaPlan("00:00:00 - Opening\n00:08:00 - Ending\n"
                             "00:04:00 - Middle\n",
                             540'000'000, &chapterPlan, &error) &&
          !parseChapterLlamaPlan("00:01:00 - Late\n", 540'000'000, &chapterPlan,
                                 &error) &&
          !parseChapterLlamaPlan(
              "00:00:00 - Opening\n00:09:00 - Outside video\n", 540'000'000,
              &chapterPlan, &error),
      "native chapter plans must reject reordering, a missing video-start "
      "identity, and timestamps outside the video");
  ok &= expect(
      parseChapterLlamaPlan("00:00:00 - Opening\n00:04:01 - Demonstration\n"
                            "00:07:59 - Result\n",
                            540'000'000, &chapterPlan, &error) &&
          chapterPlan.size() == 3 && chapterPlan[0].startUs == 0 &&
          chapterPlan[0].title == "Opening" &&
          chapterPlan[1].startUs == 241'000'000 &&
          chapterPlan[2].startUs == 479'000'000,
      "native Chapter-Llama timestamps must remain planner output rather than "
      "being snapped to unrelated sampled frames");
  ok &= expect(
      !parseChapterLlamaPlan(
          "Here are the chapters:\n00:00:00 - Opening\n00:04:00 - Result\n",
          540'000'000, &chapterPlan, &error),
      "text outside the trained Chapter-Llama output protocol must fail closed");

  GeneratedDocument generated;
  generated.chapters = {
      {0, "Opening"},
      {20'000'000, "Middle"},
      {40'000'000, "Ending"},
  };
  const AnalysisResult parsed =
      materializeGeneratedDocument(generated, 60'000'000);
  ok &=
      expect(parsed.status == OperationStatus::Succeeded &&
                 parsed.chapters.size() == 3 &&
                 parsed.chapters[1].startUs == 20'000'000 &&
                 parsed.chapters.back().endUs == 60'000'000,
             "generated planner timestamps must form a valid partition");
  GeneratedDocument repeatedTitle = generated;
  for (GeneratedChapter &chapter : repeatedTitle.chapters) {
    chapter.title = "Gameplay";
  }
  ok &= expect(materializeGeneratedDocument(repeatedTitle, 60'000'000).status ==
                   OperationStatus::Succeeded,
               "the final adapter may legitimately repeat a concise title");
  GeneratedDocument duplicate = generated;
  duplicate.chapters[1].startUs = 0;
  ok &= expect(materializeGeneratedDocument(duplicate, 60'000'000).status ==
                   OperationStatus::Failed,
               "duplicate model-selected frame identities must be rejected, "
               "not silently deduplicated");
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
      !navigationTarget(snapshot, 2'000'000, NavigationDirection::Previous) &&
          navigationTarget(snapshot, 10'000'000,
                           NavigationDirection::Previous) == 0 &&
          navigationTarget(snapshot, 25'000'000,
                           NavigationDirection::Previous) == 0 &&
          navigationTarget(snapshot, 26'000'000,
                           NavigationDirection::Previous) == 20'000'000 &&
          navigationTarget(snapshot, 25'000'000, NavigationDirection::Next) ==
              40'000'000 &&
          !navigationTarget(snapshot, 50'000'000, NavigationDirection::Next),
      "previous-chapter navigation must restart the current section after "
      "five seconds and otherwise move to the adjacent section");

  const MarkerProjection markers = projectMarkers(snapshot, 7);
  ok &= expect(markers.boundaryCells == std::vector<int>({2, 4}) &&
                   markers.collisionCells.empty(),
               "chapter boundaries must project proportionally");
  const auto editedSnapshot = projectToPresentationTimeline(
      snapshot, 40'000'000,
      {{0, 10'000'000, 0}, {30'000'000, 60'000'000, 10'000'000}});
  const MarkerProjection editedMarkers =
      editedSnapshot ? projectMarkers(*editedSnapshot, 9) : MarkerProjection{};
  ok &= expect(
      editedSnapshot && editedSnapshot->durationUs == 40'000'000 &&
          editedSnapshot->chapters.size() == 3 &&
          editedSnapshot->chapters[0].startUs == 0 &&
          editedSnapshot->chapters[0].endUs == 10'000'000 &&
          editedSnapshot->chapters[1].startUs == 10'000'000 &&
          editedSnapshot->chapters[1].endUs == 20'000'000 &&
          editedSnapshot->chapters[2].startUs == 20'000'000 &&
          editedSnapshot->chapters[2].endUs == 40'000'000 &&
          editedMarkers.boundaryCells == std::vector<int>({2, 4}) &&
          editedMarkers.collisionCells.empty() &&
          previewMetadata(*editedSnapshot, 15'000'000).back() ==
              "0:10 - 0:20",
      "markers, hover ranges and navigation must share one chapter partition "
      "projected through removed source ranges");
  snapshot.chapters = {{1, 0, 10, "A"},
                       {2, 10, 11, "B"},
                       {3, 11, 12, "C"},
                       {4, 12, 1000, "D"}};
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
  const auto wideChapterRows =
      std::count_if(wide.lines.begin(), wide.lines.end(),
                    [](const OverviewPanelLayout::Line &line) {
                      return line.chapterStartUs.has_value();
                    });
  const auto secondWideChapter =
      std::find_if(wide.lines.begin(), wide.lines.end(),
                   [](const OverviewPanelLayout::Line &line) {
                     return line.chapterStartUs == 20'000'000;
                   });
  ok &= expect(static_cast<std::size_t>(wideChapterRows) ==
                       snapshot.chapters.size() &&
                   secondWideChapter != wide.lines.end(),
               "each visible chapter row must retain its navigation target");
  ok &= expect(std::none_of(wide.lines.begin(), wide.lines.end(),
                             [](const OverviewPanelLayout::Line &line) {
                               return panelLineText(line) ==
                                      "Chapter analysis complete";
                             }),
               "a ready chapter panel must present content rather than a redundant "
               "lifecycle status");
  Snapshot verbose = snapshot;
  verbose.chapters[1].title =
      "A deliberately long chapter title that must continue on another line";
  const OverviewPanelLayout wrappedOverview =
      layoutOverviewPanel(verbose, 120, 40, 35);
  bool overviewWidthsValid = wrappedOverview.drawable();
  for (const OverviewPanelLayout::Line &line : wrappedOverview.lines) {
    overviewWidthsValid =
        overviewWidthsValid &&
        utf8DisplayWidth(panelLineText(line)) <= wrappedOverview.width - 4;
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
                      [&](const OverviewPanelLayout::Line &line) {
                        const std::string text = panelLineText(line);
                        return text.size() >= continuationIndent.size() &&
                               text.compare(0, continuationIndent.size(),
                                            continuationIndent) == 0 &&
                               text.find("continue") != std::string::npos &&
                               line.runs.size() == 1 &&
                               line.runs.front().role ==
                                   OverviewPanelLayout::TextRole::Accent;
                      }),
      "chapter titles must wrap to the actual drawer width "
      "without losing title decoration");
  Snapshot manyChapters;
  manyChapters.state = AnalysisState::Ready;
  manyChapters.durationUs = 120'000'000;
  for (std::size_t index = 0; index < 12; ++index) {
    manyChapters.chapters.push_back(
        {index + 1, static_cast<std::int64_t>(index) * 10'000'000,
         static_cast<std::int64_t>(index + 1) * 10'000'000,
         "Chapter title " + std::to_string(index + 1)});
  }
  const OverviewPanelLayout firstPage =
      layoutOverviewPanel(manyChapters, 100, 16, 14);
  const OverviewPanelLayout lastPage = layoutOverviewPanel(
      manyChapters, 100, 16, 14, firstPage.maximumScrollOffset);
  ok &= expect(
      firstPage.drawable() && firstPage.maximumScrollOffset > 0 &&
          lastPage.scrollOffset == lastPage.maximumScrollOffset &&
          std::any_of(lastPage.lines.begin(), lastPage.lines.end(),
                      [](const OverviewPanelLayout::Line &line) {
                         return panelLineText(line).find(
                                   "Chapter title 12") != std::string::npos;
                      }),
      "chapter-panel scrolling must make the final generated chapter reachable");
  const auto compactPreview = playback_video_timeline_preview::layoutCells(
      50, 10, 6, 1, 48, 0.5, 1920, 1080, 9.0, 21.0, "00:30",
      {"Chapter 2 of 3", "00:20 - 00:40"});
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
       "00:20 - 00:40"});
  bool previewWidthsValid = wrappedPreview.metadataLines.size() > 2;
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

  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path testDir =
      std::filesystem::temp_directory_path() /
      ("radioify-chapter-text-" + std::to_string(stamp));
  std::error_code filesystemError;
  std::filesystem::create_directories(testDir, filesystemError);
  const std::filesystem::path video = testDir / "movie.webm";
  const std::filesystem::path generated =
      playback_video_transcript::generatedEnglishTranscriptPathForVideo(video);
  const std::string generatedProducer = "radioify-test-producer-v1";
  std::string generatedError;
  {
    std::ofstream source(video, std::ios::binary);
    source << "representative media identity";
  }
  const auto sourceIdentity =
      playback_video_transcript::captureTranscriptSourceIdentity(
          video, &generatedError);
  auto generatedTransaction = file_output::TransactionGroup::begin(
      {{generated, file_output::PublishMode::CreateNew},
       {playback_video_transcript::transcriptProvenancePath(generated),
        file_output::PublishMode::CreateNew}},
      &generatedError);
  const bool generatedPublished =
      sourceIdentity && generatedTransaction &&
      playback_video_transcript::writeIndexedTranscriptStaging(
          generatedTransaction->temporaryPath(0),
          {{1'000'000, 2'000'000, "Persisted speech"}}, &generatedError) &&
      playback_video_transcript::writeTranscriptProvenanceStaging(
          *sourceIdentity, generatedProducer, generated,
          generatedTransaction->temporaryPath(0),
          generatedTransaction->temporaryPath(1), &generatedError) &&
      generatedTransaction->publish(&generatedError);
  ok &= expect(!filesystemError && generatedPublished,
               "the generated-English evidence fixture must be publishable");
  const auto generatedEvidence =
      loadGeneratedEnglishTextEvidence(video, generatedProducer,
                                       &generatedError);
  ok &= expect(!loadGeneratedEnglishTextEvidence(
                   video, "radioify-test-producer-v2"),
               "generated English evidence must be invalidated when its "
               "model or transcript algorithm identity changes");
  const auto rediscoveredEvidence =
      loadGeneratedEnglishTextEvidence(video, generatedProducer);
  ok &= expect(generatedEvidence && generatedError.empty() &&
                   generatedEvidence->language == "en" &&
                   generatedEvidence->cues.size() == 1 &&
                   generatedEvidence->cues.front().text == "Persisted speech" &&
                   rediscoveredEvidence &&
                   generatedEvidence->identity == rediscoveredEvidence->identity,
               "chapter analysis must consume an English ASR sidecar created "
               "after subtitle discovery with the same cache identity used "
               "after reopening playback");
#ifdef _WIN32
  std::wstring caseVariantText = video.wstring();
  if (!caseVariantText.empty() && caseVariantText.front() >= L'A' &&
      caseVariantText.front() <= L'Z') {
    caseVariantText.front() =
        static_cast<wchar_t>(caseVariantText.front() - L'A' + L'a');
  }
  const std::filesystem::path caseVariantVideo(caseVariantText);
  const auto caseVariantEvidence =
      loadGeneratedEnglishTextEvidence(caseVariantVideo, generatedProducer);
  ok &= expect(caseVariantEvidence.has_value(),
               "Windows path casing must not invalidate generated transcript "
               "provenance");
  if (generatedEvidence && caseVariantEvidence) {
    AnalysisRequest originalRequest;
    originalRequest.file = video;
    originalRequest.durationUs = 10'000'000;
    originalRequest.englishText = generatedEvidence;
    AnalysisRequest caseVariantRequest = originalRequest;
    caseVariantRequest.file = caseVariantVideo;
    caseVariantRequest.englishText = caseVariantEvidence;
    ok &= expect(analysisSourceKey(originalRequest) ==
                     analysisSourceKey(caseVariantRequest),
                 "Windows path casing must not create a second chapter cache "
                 "identity");
  }
#endif
  const std::uintmax_t originalSize =
      std::filesystem::file_size(video, filesystemError);
  const auto originalTime =
      std::filesystem::last_write_time(video, filesystemError);
  const std::filesystem::path replacement = testDir / "replacement.webm";
  {
    std::ofstream changedSource(replacement, std::ios::binary);
    changedSource << std::string(static_cast<std::size_t>(originalSize), 'x');
  }
  std::filesystem::remove(video, filesystemError);
  std::filesystem::rename(replacement, video, filesystemError);
  std::filesystem::last_write_time(video, originalTime, filesystemError);
  ok &= expect(!loadGeneratedEnglishTextEvidence(video, generatedProducer),
               "a generated transcript must not survive same-size, same-time "
               "replacement of its source video at the same path");
  const auto replacementIdentity =
      playback_video_transcript::captureTranscriptSourceIdentity(
          video, &generatedError);
  auto repairTransaction = file_output::TransactionGroup::begin(
      {{generated, file_output::PublishMode::ReplaceExisting},
       {playback_video_transcript::transcriptProvenancePath(generated),
        file_output::PublishMode::ReplaceExisting}},
      &generatedError);
  const bool repaired =
      replacementIdentity && repairTransaction &&
      playback_video_transcript::writeIndexedTranscriptStaging(
          repairTransaction->temporaryPath(0),
          {{2'000'000, 3'000'000, "Replacement speech"}}, &generatedError) &&
      playback_video_transcript::writeTranscriptProvenanceStaging(
          *replacementIdentity, generatedProducer, generated,
          repairTransaction->temporaryPath(0),
          repairTransaction->temporaryPath(1), &generatedError) &&
      repairTransaction->publish(&generatedError);
  const auto repairedEvidence =
      loadGeneratedEnglishTextEvidence(video, generatedProducer);
  ok &= expect(repaired && repairedEvidence &&
                   repairedEvidence->cues.front().text ==
                       "Replacement speech",
               "a stale Radioify-owned transcript pair must be repairable as "
               "one atomic publication after source replacement");
  std::filesystem::remove_all(testDir, filesystemError);
  return ok;
}

} // namespace

int main() {
  const bool ok = runChapterDomainTests() && runTextEvidenceTests();
  return ok ? 0 : 1;
}
