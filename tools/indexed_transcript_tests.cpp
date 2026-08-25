#include "playback/video/transcript/document.h"
#include "tui/ui/file_context_menu_model.h"
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
  ok &= expect(transcript::defaultTranscriptPath("film.mkv") ==
                   std::filesystem::path("film.transcript.srt"),
               "default sidecar must retain the video stem");
  ok &= expect(transcript::defaultTranscriptPath("archive.name.mp4") ==
                   std::filesystem::path("archive.name.transcript.srt"),
               "default sidecar must retain multi-dot stems");

  FileContextMenuCapabilities videoCapabilities;
  videoCapabilities.video = true;
  const std::vector<FileContextMenuItem> videoItems =
      buildFileContextMenuItems(videoCapabilities);
  ok &= expect(videoItems.size() == 3 &&
                   videoItems[2].action ==
                       FileContextAction::CreateIndexedTranscript,
               "idle video menus must expose indexed transcripts");
  FileContextMenuCapabilities audioCapabilities;
  audioCapabilities.audio = true;
  const std::vector<FileContextMenuItem> audioItems =
      buildFileContextMenuItems(audioCapabilities);
  ok &= expect(std::none_of(audioItems.begin(), audioItems.end(),
                            [](const FileContextMenuItem& item) {
                              return item.action ==
                                     FileContextAction::CreateIndexedTranscript;
                            }),
               "audio-only menus must not expose video transcription");
  videoCapabilities.backgroundTaskRunning = true;
  ok &= expect(buildFileContextMenuItems(videoCapabilities).size() == 2,
               "a running background task must suppress duplicate work");
  const BrowserFooterLayout transcriptFooter = computeBrowserFooterLayout(
      true, false, false, false, true, false, false, false);
  ok &= expect(transcriptFooter.showTranscriptStatus &&
                   transcriptFooter.reservedLines == 2,
               "a completed transcript must reserve a visible status row");

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
  ok &= expect(transcript::availableTranscriptPath(testDir / "film.mkv") ==
                   output,
               "the preferred transcript path must be selected when free");
  std::string error;
  const std::vector<transcript::Segment> segments = {
      {2'345'000, 4'000'000, "  tweede\r\nregel  "},
      {0, 1'234'000, " Eerste regel "},
      {5'000'000, 5'000'000, "   "},
      {6'000'000, 7'000'000, " ... -- "},
      {8'000'000, 9'000'000, " [BLANK_AUDIO] "},
  };
  ok &= expect(transcript::writeIndexedTranscript(output, segments, &error),
               "valid cues must be written");
  if (!error.empty()) std::cerr << error << '\n';
  const std::string expected =
      "1\r\n00:00:00,000 --> 00:00:01,234\r\nEerste regel\r\n\r\n"
      "2\r\n00:00:02,345 --> 00:00:04,000\r\ntweede regel\r\n\r\n";
  ok &= expect(readFile(output) == expected,
                "SRT output must be sorted, indexed, whitespace-normalized, "
                "and omit silence hallucinations");

  ok &= expect(transcript::availableTranscriptPath(testDir / "film.mkv") ==
                   testDir / "film.transcript.2.srt",
               "a new transcript must not overwrite an existing sidecar");
  const std::vector<transcript::Segment> replacement = {
      {60'000'000, 61'000'000, "vervangen"},
  };
  const std::string beforeReplacement = readFile(output);
  ok &= expect(!transcript::writeIndexedTranscript(output, replacement, &error),
               "an existing transcript must not be overwritten");
  ok &= expect(readFile(output) == beforeReplacement,
               "a rejected replacement must preserve the existing transcript");

  const std::filesystem::path emptyOutput = testDir / "empty.transcript.srt";
  ok &= expect(!transcript::writeIndexedTranscript(emptyOutput, {}, &error),
               "an empty transcript must be rejected");
  ok &= expect(!std::filesystem::exists(emptyOutput),
               "a failed empty transcript must not leave an output file");

  std::filesystem::remove_all(testDir, ec);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
