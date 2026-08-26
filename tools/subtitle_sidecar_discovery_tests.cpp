#include "playback/video/subtitle/sidecar_discovery.h"
#include "playback/video/sidecar_identity.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "subtitle_sidecar_discovery_tests: " << message << '\n';
  return false;
}

void touch(const std::filesystem::path& path) {
  std::ofstream file(path, std::ios::binary);
  file << "1\n00:00:00,000 --> 00:00:01,000\nTest\n";
}

int inspectRealPath(const std::filesystem::path& videoPath) {
  const auto sidecars =
      playback_video_subtitle::discoverAutomaticSubtitleSidecars(videoPath);
  std::cout << "subtitle_sidecar_discovery: " << sidecars.size()
            << " automatic sidecar(s)\n";
  for (const auto& sidecar : sidecars) {
    std::cout << sidecar.string() << '\n';
  }
  return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
  namespace subtitle = playback_video_subtitle;
  if (argc == 2) return inspectRealPath(std::filesystem::path(argv[1]));

  bool ok = true;
  ok &= expect(subtitle::isAutomaticSubtitleSidecar(
                   "episode.mp4", "episode.srt"),
               "an exact stem must be accepted");
  ok &= expect(subtitle::isAutomaticSubtitleSidecar(
                   "episode.mp4", "episode.transcript.4.srt"),
               "a numbered Radioify transcript must be accepted");
  ok &= expect(playback_video_sidecars::isIndexedTranscriptSidecar(
                   "episode.mp4", "episode.transcript.4.srt") &&
                   !playback_video_sidecars::isIndexedTranscriptSidecar(
                       "episode.mp4", "episode.transcript.4.ass") &&
                   subtitle::isAutomaticSubtitleSidecar(
                       "episode.mp4", "episode.transcript.4.ass"),
               "only SRT transcript sidecars are indexed, while converted "
               "subtitle formats remain discoverable");
  ok &= expect(subtitle::isAutomaticSubtitleSidecar(
                   "episode.mp4", "episode.en-US.forced.ass"),
               "language and role qualifiers must be accepted");
  ok &= expect(!subtitle::isAutomaticSubtitleSidecar(
                   "episode.mp4", "episode2.srt") &&
                   !subtitle::isAutomaticSubtitleSidecar(
                       "episode.mp4", "episode Part 2.srt") &&
                   !subtitle::isAutomaticSubtitleSidecar(
                       "episode.mp4", "episode.Part2.srt") &&
                   !subtitle::isAutomaticSubtitleSidecar(
                       "episode.mp4", "other.transcript.srt"),
               "similar or unrelated media stems must never match");

  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("radioify-subtitle-sidecars-" + std::to_string(stamp));
  std::error_code ec;
  std::filesystem::create_directories(directory / "subs", ec);
  if (ec) {
    std::cerr << "subtitle_sidecar_discovery_tests: could not create fixture\n";
    return EXIT_FAILURE;
  }

  const std::filesystem::path video = directory / "episode.mp4";
  touch(video);
  touch(directory / "other.transcript.srt");
  touch(directory / "English.srt");
  touch(directory / "episode Part 2.srt");
  touch(directory / "episode.transcript.srt");
  touch(directory / "episode.transcript.2.srt");
  touch(directory / "subs" / "episode.nl.forced.srt");
  const std::vector<std::filesystem::path> sidecars =
      subtitle::discoverAutomaticSubtitleSidecars(video);
  ok &= expect(sidecars.size() == 2,
               "discovery must retain only exact owned sidecars across search "
               "directories");
  ok &= expect(std::find(sidecars.begin(), sidecars.end(),
                         directory / "episode.transcript.srt") !=
                       sidecars.end() &&
                   std::find(sidecars.begin(), sidecars.end(),
                             directory / "subs" /
                                 "episode.nl.forced.srt") != sidecars.end(),
               "the exact main-directory and subtitle-directory files must be "
               "returned");
  ok &= expect(std::find(sidecars.begin(), sidecars.end(),
                         directory / "episode.transcript.2.srt") ==
                   sidecars.end(),
               "legacy transcript versions must not become extra tracks");

  ec.clear();
  std::filesystem::remove(directory / "episode.transcript.srt", ec);
  const std::vector<std::filesystem::path> legacySidecars =
      subtitle::discoverAutomaticSubtitleSidecars(video);
  ok &= expect(!ec && legacySidecars.size() == 2 &&
                   std::find(legacySidecars.begin(), legacySidecars.end(),
                             directory / "episode.transcript.2.srt") !=
                       legacySidecars.end(),
               "the legacy fallback must be shared with subtitle discovery");

  std::filesystem::remove_all(directory, ec);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
