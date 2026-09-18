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
#include "playback/video/analysis/text_evidence.h"
#include "playback/video/analysis/visual_timeline_scan.h"
#include "playback/video/subtitle/manager.h"
#include "playback/video/timeline_preview_types.h"
#include "playback/video/transcript/artifact.h"
#include "playback/video/transcript/document.h"
#include "playback/video/transcript/source_identity.h"

namespace {

bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "speech_evidence_tests: " << message << '\n';
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

bool runTextEvidenceTests() {
  using namespace playback_video_analysis;
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
  std::string initialIdentity = evidence ? evidence->identity : std::string{};
  tracks.back().cues.back().text = "The dialogue content changed";
  const auto changedEvidence = selectEnglishTextEvidence(tracks, "movie.mkv");
  ok &= expect(changedEvidence && !initialIdentity.empty() &&
                   changedEvidence->identity != initialIdentity,
               "analysis cache identity must address the exact timed-text "
               "content instead of file metadata");

  tracks.erase(tracks.begin() + 3);
  ok &= expect(!selectEnglishTextEvidence(tracks, "movie.mkv"),
               "forced and commentary tracks must not impersonate complete "
               "dialogue evidence");
  SubtitleTrack sdh = track("English SDH", "en", false, false, {intro, body});
  sdh.hearingImpaired = true;
  tracks.push_back(std::move(sdh));
  const auto fallback = selectEnglishTextEvidence(tracks, "movie.mkv");
  ok &= expect(fallback && fallback->label == "English SDH",
               "a complete English SDH track must remain valid evidence");

  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path testDir =
      std::filesystem::temp_directory_path() /
      ("radioify-speech-evidence-" + std::to_string(stamp));
  std::error_code filesystemError;
  std::filesystem::create_directories(testDir, filesystemError);
  const std::filesystem::path video = testDir / "movie.webm";
  const std::filesystem::path generated =
      playback_video_transcript::generatedEnglishTranscriptPathForVideo(video);
  std::string generatedError;
  {
    std::ofstream source(video, std::ios::binary);
    source << "representative media identity";
  }
  const auto sourceIdentity =
      playback_video_transcript::captureTranscriptSourceIdentity(
          video, &generatedError);
  const bool generatedPublished =
      playback_video_transcript::writeIndexedTranscript(
          generated, {{1'000'000, 2'000'000, "Persisted speech"}},
          playback_video_transcript::TranscriptPublishMode::CreateNew,
          &generatedError);
  ok &= expect(!filesystemError && generatedPublished,
               "the generated-English evidence fixture must be publishable");
  const auto generatedEvidence =
      loadGeneratedEnglishTextEvidence(video, &generatedError);
  size_t fileCount = 0;
  for (const auto &entry : std::filesystem::directory_iterator(testDir)) {
    ++fileCount;
    ok &= expect(
        entry.path() == video || entry.path() == generated,
        "a transcript must not create companion manifests or control files");
  }
  ok &= expect(fileCount == 2,
               "transcript publication must leave only the video and SRT");
  const auto rediscoveredEvidence = loadGeneratedEnglishTextEvidence(video);
  ok &=
      expect(generatedEvidence && generatedError.empty() &&
                 generatedEvidence->language == "en" &&
                 generatedEvidence->cues.size() == 1 &&
                 generatedEvidence->cues.front().text == "Persisted speech" &&
                 rediscoveredEvidence &&
                 generatedEvidence->identity == rediscoveredEvidence->identity,
             "editor analysis must consume an English ASR sidecar created "
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
      loadGeneratedEnglishTextEvidence(caseVariantVideo);
  ok &= expect(caseVariantEvidence.has_value(),
               "Windows path casing must not prevent transcript discovery");
#endif
  {
    const auto transcriptTime =
        std::filesystem::last_write_time(generated, filesystemError);
    ok &= expect(
        playback_video_transcript::writeIndexedTranscript(
            generated, {{1'000'000, 2'000'000, "Corrected speech"}},
            playback_video_transcript::TranscriptPublishMode::ReplaceExisting,
            &generatedError),
        "the transcript document may be corrected independently of its "
        "producer");
    std::filesystem::last_write_time(generated, transcriptTime,
                                     filesystemError);
  }
  const auto correctedEvidence = loadGeneratedEnglishTextEvidence(video);
  ok &= expect(correctedEvidence && generatedEvidence &&
                   correctedEvidence->cues.front().text == "Corrected speech" &&
                   correctedEvidence->identity != generatedEvidence->identity,
               "transcript edits must be consumed as documents and change "
               "downstream analysis cache identity");
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
  ok &= expect(sourceIdentity &&
                   !playback_video_transcript::transcriptSourceMatches(
                       *sourceIdentity, video),
               "an in-flight transcription must detect source replacement even "
               "at the same size and time");
  const bool repaired = playback_video_transcript::writeIndexedTranscript(
      generated, {{2'000'000, 3'000'000, "Replacement speech"}},
      playback_video_transcript::TranscriptPublishMode::ReplaceExisting,
      &generatedError);
  const auto repairedEvidence = loadGeneratedEnglishTextEvidence(video);
  ok &= expect(
      repaired && repairedEvidence &&
          repairedEvidence->cues.front().text == "Replacement speech",
      "explicit retranscription must atomically replace one SRT document");
  std::filesystem::remove_all(testDir, filesystemError);
  return ok;
}

} // namespace

int main() {
  const bool ok = runTextEvidenceTests();
  return ok ? 0 : 1;
}
