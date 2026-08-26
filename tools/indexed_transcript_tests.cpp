#include "playback/video/transcript/document.h"
#include "playback/video/transcript/artifact.h"
#include "playback/video/transcript/cue_semantics.h"
#include "playback/video/transcript/device_selection.h"
#include "playback/video/transcript/subtitle_cues.h"
#include "playback/video/transcript/whisper_model_config.h"
#include "tui/ui/ui_footer_layout.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "indexed_transcript_tests: " << message << '\n';
  return false;
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
  namespace transcript = playback_video_transcript;
  bool ok = true;

  ok &= expect(transcript::parseWhisperAlignmentPreset(" base.en ") ==
                   transcript::WhisperAlignmentPreset::BaseEn &&
                   transcript::parseWhisperAlignmentPreset(
                       "LARGE-V3-TURBO") ==
                       transcript::WhisperAlignmentPreset::LargeV3Turbo &&
                   transcript::parseWhisperAlignmentPreset("off") ==
                       transcript::WhisperAlignmentPreset::None &&
                   !transcript::parseWhisperAlignmentPreset("my-base-model"),
               "DTW presets must be explicit values, never filename guesses");
  ok &= expect(transcript::isTranscriptSoundAnnotation(" [thunder] ") &&
                   !transcript::isTranscriptSoundAnnotation(
                       "(Welcome honored guests)") &&
                   !transcript::isTranscriptSoundAnnotation("spoken text"),
               "only square-bracketed SDH cues must be non-speech annotations");
  ok &= expect(!transcript::isTranscriptSpeechCue(" [Music] ") &&
                   transcript::isTranscriptSpeechCue(
                       "(Welcome honored guests)") &&
                   transcript::isTranscriptSpeechCue("spoken text"),
               "speech-only policy must reject annotations without treating "
               "all parenthetical text as sound");

  const std::vector<transcript::VulkanDeviceCandidate> gpuCandidates = {
      {0, transcript::VulkanDeviceClass::Integrated, 12, 24, "Vulkan0",
       "Integrated GPU"},
      {1, transcript::VulkanDeviceClass::Discrete, 4, 8, "Vulkan1",
       "Discrete GPU"},
      {2, transcript::VulkanDeviceClass::Discrete, 10, 16, "Vulkan2",
       "Larger discrete GPU"},
  };
  const auto selectedGpu =
      transcript::selectPreferredVulkanDevice(gpuCandidates);
  ok &= expect(selectedGpu && selectedGpu->whisperGpuIndex == 2,
               "Vulkan policy must prefer a discrete GPU, then its total "
               "memory");
  const std::vector<transcript::VulkanDeviceCandidate> equalMemoryGpus = {
      {4, transcript::VulkanDeviceClass::Discrete, 6, 16, "Vulkan4", {}},
      {3, transcript::VulkanDeviceClass::Discrete, 8, 16, "Vulkan3", {}},
  };
  const auto gpuWithMoreFreeMemory =
      transcript::selectPreferredVulkanDevice(equalMemoryGpus);
  ok &= expect(gpuWithMoreFreeMemory &&
                   gpuWithMoreFreeMemory->whisperGpuIndex == 3,
               "equally sized Vulkan GPUs must prefer available memory");
  ok &= expect(!transcript::selectPreferredVulkanDevice({}),
               "an empty Vulkan device list must not select a GPU");

  const std::vector<transcript::RecognizedSegment> timedRecognition = {
      {0,
       20'000'000,
       " Hello world. Much later.",
       {{0, 1'000'000, -1, " Hello"},
        {1'000'000, 2'000'000, -1, " world."},
        {10'000'000, 11'000'000, -1, " Much"},
        {11'000'000, 12'000'000, -1, " later."}}},
  };
  const std::vector<transcript::Segment> timedCues =
      transcript::buildSubtitleCues(timedRecognition);
  ok &= expect(timedCues.size() == 2 &&
                   timedCues[0].startUs == 0 &&
                   timedCues[0].endUs == 2'150'000 &&
                   timedCues[1].startUs == 10'000'000 &&
                   timedCues[1].endUs == 12'150'000,
               "word timing and real silence must own subtitle boundaries");

  const std::vector<transcript::RecognizedSegment> alignedRecognition = {
      {0,
       20'000'000,
       " Precisely aligned.",
       {{0, 10'000'000, 5'000'000, " Precisely"},
        {10'000'000, 20'000'000, 6'000'000, " aligned."}}},
  };
  const std::vector<transcript::Segment> alignedCues =
      transcript::buildSubtitleCues(alignedRecognition);
  ok &= expect(alignedCues.size() == 1 &&
                   alignedCues[0].startUs == 4'800'000 &&
                   alignedCues[0].endUs == 6'500'000,
               "DTW alignment must override coarse decoder token spans");

  const std::vector<transcript::RecognizedSegment> untimedRecognition = {
      {0,
       10'000'000,
       " Hello, world.",
       {{0, 1'000'000, 500'000, " Hello"},
        {-1, -1, 9'000'000, ","},
        {1'000'000, 2'000'000, 1'000'000, " world."}}},
  };
  const std::vector<transcript::Segment> untimedCues =
      transcript::buildSubtitleCues(untimedRecognition);
  ok &= expect(untimedCues.size() == 1 &&
                   untimedCues[0].text == " Hello, world." &&
                   untimedCues[0].startUs == 300'000 &&
                   untimedCues[0].endUs == 1'500'000,
               "an untimed lexical token must not leak alignment into the "
               "next word");

  const std::vector<transcript::RecognizedSegment> sentenceRecognition = {
      {0,
       10'000'000,
       " Welcome, honored guests! I hope everyone attended you well. Honored guests?",
       {{0, 500'000, -1, " Welcome,"},
        {500'000, 1'000'000, -1, " honored"},
        {1'000'000, 1'500'000, -1, " guests!"},
        {1'500'000, 2'000'000, -1, " I"},
        {2'000'000, 2'500'000, -1, " hope"},
        {2'500'000, 3'000'000, -1, " everyone"},
        {3'000'000, 3'500'000, -1, " attended"},
        {3'500'000, 4'000'000, -1, " you"},
        {4'000'000, 4'500'000, -1, " well."},
        {4'500'000, 5'000'000, -1, " Honored"},
        {5'000'000, 5'500'000, -1, " guests?"}}},
  };
  const std::vector<transcript::Segment> sentenceCues =
      transcript::buildSubtitleCues(sentenceRecognition);
  ok &= expect(sentenceCues.size() == 2 &&
                   sentenceCues[0].text.back() == '.' &&
                   sentenceCues[1].text == " Honored guests?",
               "natural sentence boundaries must prevent orphan words");

  const std::vector<transcript::RecognizedSegment> utf8Recognition = {
      {0,
       2'000'000,
       " €",
       {{0, 500'000, -1, " \xE2"},
        {500'000, 1'000'000, -1, "\x82\xAC"}}},
  };
  const std::vector<transcript::Segment> utf8Cues =
      transcript::buildSubtitleCues(utf8Recognition);
  ok &= expect(utf8Cues.size() == 1 && utf8Cues[0].text == " €",
               "token byte fragments must not create invalid UTF-8 cues");

  const std::vector<transcript::RecognizedSegment> annotationRecognition = {
      {0,
       30'000'000,
       " [thunder rumble]",
       {{0, 10'000'000, 12'000'000, " [th"},
        {10'000'000, 20'000'000, 12'100'000, "under"},
        {20'000'000, 30'000'000, 13'000'000, " rumble]"}}},
  };
  const std::vector<transcript::Segment> annotationCues =
      transcript::buildSubtitleCues(annotationRecognition);
  ok &= expect(annotationCues.empty(),
               "generated subtitle cues must omit sound annotations");

  std::vector<transcript::Segment> overlappingCues = {
      {0, 8'000'000, "First"},
      {5'000'000, 6'000'000, "Second"},
  };
  transcript::finalizeSubtitleCueTimeline(&overlappingCues);
  ok &= expect(overlappingCues.size() == 2 &&
                   overlappingCues[0].endUs == 5'000'000,
               "a successor cue must end an older overlapping cue");

  ok &= expect(transcript::transcriptPathForVideo("film.mkv") ==
                   std::filesystem::path("film.transcript.srt"),
               "canonical sidecar must retain the video stem");
  ok &= expect(transcript::transcriptPathForVideo("archive.name.mp4") ==
                   std::filesystem::path("archive.name.transcript.srt"),
               "canonical sidecar must retain multi-dot stems");

  const BrowserFooterLayout subtitleGenerationFooter =
      computeBrowserFooterLayout(true, false, false, false, true, false,
                                 false, false, false);
  ok &= expect(subtitleGenerationFooter.showSubtitleGenerationStatus &&
                   subtitleGenerationFooter.reservedLines == 2,
               "completed subtitle generation must reserve a visible status "
               "row");

  const BrowserFooterLayout audioSeparationFooter =
      computeBrowserFooterLayout(true, false, false, false, false, true,
                                 false, false, false);
  ok &= expect(audioSeparationFooter.showAudioSeparationStatus &&
                   audioSeparationFooter.reservedLines == 2,
               "completed audio separation must reserve a visible status row");

  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path testDir =
      std::filesystem::temp_directory_path() /
      ("radioify-indexed-transcript-" + std::to_string(stamp));
  std::error_code ec;
  std::filesystem::create_directories(testDir, ec);
  if (ec) {
    std::cerr << "indexed_transcript_tests: could not create test directory\n";
    return EXIT_FAILURE;
  }

  const std::filesystem::path output = testDir / "film.transcript.srt";
  ok &= expect(transcript::transcriptPathForVideo(testDir / "film.mkv") ==
                   output &&
                   transcript::activeTranscriptPathForVideo(
                       testDir / "film.mkv").empty(),
               "the canonical transcript path must not imply an artifact");
  std::string error;
  const std::vector<transcript::Segment> segments = {
      {2'345'000, 4'000'000, "  tweede\r\nregel  "},
      {0, 1'234'000, " Eerste regel "},
      {5'000'000, 5'000'000, "   "},
      {6'000'000, 7'000'000, " ... -- "},
      {8'000'000, 9'000'000, " [BLANK_AUDIO] "},
      {10'000'000, 11'000'000, " [Music] "},
      {12'000'000, 13'000'000, " (Welcome honored guests) "},
  };
  ok &= expect(transcript::writeIndexedTranscript(
                   output, segments,
                   transcript::TranscriptPublishMode::CreateNew, &error),
               "valid cues must be written");
  if (!error.empty()) std::cerr << error << '\n';
  const std::string expected =
      "1\r\n00:00:00,000 --> 00:00:01,234\r\nEerste regel\r\n\r\n"
      "2\r\n00:00:02,345 --> 00:00:04,000\r\ntweede regel\r\n\r\n"
      "3\r\n00:00:12,000 --> 00:00:13,000\r\n"
      "(Welcome honored guests)\r\n\r\n";
  ok &= expect(readFile(output) == expected,
                "SRT output must be sorted, indexed, whitespace-normalized, "
                "and omit silence hallucinations");
  std::vector<transcript::Segment> parsedSegments;
  ok &= expect(transcript::readIndexedTranscript(
                   output, &parsedSegments, &error) &&
                   parsedSegments.size() == 3 &&
                   parsedSegments[0].startUs == 0 &&
                   parsedSegments[0].endUs == 1'234'000 &&
                   parsedSegments[1].text == "tweede regel",
               "indexed transcript readers must recover normalized SRT timing "
               "and text");
  ok &= expect(transcript::activeTranscriptPathForVideo(
                   testDir / "film.mkv") == output,
               "readers must select the canonical active transcript");
  const std::filesystem::path unrelatedTranscript =
      testDir / "other.transcript.srt";
  {
    std::ofstream unrelated(unrelatedTranscript, std::ios::binary);
    unrelated << "1\r\n00:00:00,000 --> 00:00:01,000\r\nOther\r\n";
  }
  ec.clear();
  const auto outputTime = std::filesystem::last_write_time(output, ec);
  if (!ec) {
    std::filesystem::last_write_time(
        unrelatedTranscript, outputTime + std::chrono::hours(1), ec);
  }
  ok &= expect(!ec &&
                   transcript::activeTranscriptPathForVideo(
                       testDir / "film.mkv") == output,
               "newer transcripts owned by another video must be ignored");
  const std::filesystem::path longOutput =
      testDir / "long.transcript.srt";
  const int64_t hundredHoursUs = 100LL * 60 * 60 * 1'000'000;
  ok &= expect(transcript::writeIndexedTranscript(
                   longOutput,
                   {{hundredHoursUs, hundredHoursUs + 1'000'000,
                     "Long recording"}},
                   transcript::TranscriptPublishMode::CreateNew,
                   &error) &&
                   transcript::readIndexedTranscript(
                       longOutput, &parsedSegments, &error) &&
                   parsedSegments.size() == 1 &&
                   parsedSegments[0].startUs == hundredHoursUs,
               "the SRT reader must round-trip writer timestamps beyond 99 hours");

  const std::filesystem::path legacyOutput =
      testDir / "film.transcript.2.srt";
  ok &= expect(transcript::writeIndexedTranscript(
                   legacyOutput, {{40'000'000, 41'000'000, "legacy"}},
                   transcript::TranscriptPublishMode::CreateNew, &error),
               "legacy numbered fixtures must remain readable");
  ec.clear();
  std::filesystem::last_write_time(
      legacyOutput, outputTime + std::chrono::hours(2), ec);
  ok &= expect(!ec &&
                   transcript::activeTranscriptPathForVideo(
                       testDir / "film.mkv") == output,
               "the canonical transcript must win over newer legacy files");
  const std::vector<transcript::Segment> replacement = {
      {60'000'000, 61'000'000, "vervangen"},
  };
  ok &= expect(transcript::writeIndexedTranscript(
                   output, replacement,
                   transcript::TranscriptPublishMode::ReplaceExisting,
                   &error) &&
                   readFile(output).find("vervangen") != std::string::npos,
               "regeneration must atomically replace the active transcript");
  ok &= expect(!transcript::writeIndexedTranscript(
                   output, segments,
                   transcript::TranscriptPublishMode::CreateNew, &error),
               "create-new publication must still protect existing files");

  ec.clear();
  std::filesystem::remove(output, ec);
  ok &= expect(!ec &&
                   transcript::activeTranscriptPathForVideo(
                       testDir / "film.mkv") == legacyOutput,
               "the newest legacy transcript must be a migration fallback");

  const std::filesystem::path emptyOutput = testDir / "empty.transcript.srt";
  ok &= expect(!transcript::writeIndexedTranscript(
                   emptyOutput, {},
                   transcript::TranscriptPublishMode::CreateNew, &error),
               "an empty transcript must be rejected");
  ok &= expect(!std::filesystem::exists(emptyOutput),
               "a failed empty transcript must not leave an output file");

  std::filesystem::remove_all(testDir, ec);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
