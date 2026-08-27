#include "audio/audio_export.h"
#include "audio/ffmpegaudio.h"
#include "playback/video/sidecar_identity.h"
#include "playback/video/transcript/artifact.h"
#include "playback/video/transcript/document.h"
#include "playback/video/transcript/text_export.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "media_export_tests: " << message << '\n';
  return false;
}

void writeU16(std::ofstream& output, std::uint16_t value) {
  const char bytes[] = {static_cast<char>(value & 0xff),
                        static_cast<char>((value >> 8) & 0xff)};
  output.write(bytes, sizeof(bytes));
}

void writeU32(std::ofstream& output, std::uint32_t value) {
  const char bytes[] = {
      static_cast<char>(value & 0xff),
      static_cast<char>((value >> 8) & 0xff),
      static_cast<char>((value >> 16) & 0xff),
      static_cast<char>((value >> 24) & 0xff)};
  output.write(bytes, sizeof(bytes));
}

bool writeMonoWave(const std::filesystem::path& path) {
  constexpr std::uint32_t kSampleRate = 44100;
  constexpr std::uint32_t kFrames = kSampleRate / 4;
  constexpr std::uint16_t kChannels = 1;
  constexpr std::uint16_t kBitsPerSample = 16;
  constexpr std::uint32_t kDataBytes = kFrames * sizeof(std::int16_t);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write("RIFF", 4);
  writeU32(output, 36 + kDataBytes);
  output.write("WAVEfmt ", 8);
  writeU32(output, 16);
  writeU16(output, 1);
  writeU16(output, kChannels);
  writeU32(output, kSampleRate);
  writeU32(output, kSampleRate * kChannels * kBitsPerSample / 8);
  writeU16(output, kChannels * kBitsPerSample / 8);
  writeU16(output, kBitsPerSample);
  output.write("data", 4);
  writeU32(output, kDataBytes);
  for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
    constexpr double kPi = 3.14159265358979323846;
    const double phase = 2.0 * kPi * 440.0 * frame / kSampleRate;
    const auto sample = static_cast<std::int16_t>(
        std::round(std::sin(phase) * 12000.0));
    writeU16(output, static_cast<std::uint16_t>(sample));
  }
  return static_cast<bool>(output);
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

bool hasTemporaryOutput(const std::filesystem::path& directory) {
  std::error_code error;
  for (const auto& entry :
       std::filesystem::directory_iterator(directory, error)) {
    if (error) return true;
    if (entry.path().filename().wstring().find(L".radioify-") !=
        std::wstring::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  bool ok = true;
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("radioify-media-export-tests-" + std::to_string(stamp));
  std::filesystem::create_directories(directory);

  const std::filesystem::path wave = directory / "source.wav";
  ok &= expect(writeMonoWave(wave), "the audio fixture must be writable");
  const std::filesystem::path flac =
      audio_export::uniqueOutputPathFor(wave);
  float latestProgress = 0.0f;
  std::string error;
  ok &= expect(audio_export::exportToFlac(
                   wave, flac,
                   [&](float progress, std::string) {
                     latestProgress = progress;
                   },
                   []() { return false; }, &error),
               "audio export must succeed");
  FfmpegAudioStreamFormat exportedFormat;
  ok &= expect(std::filesystem::is_regular_file(flac) &&
                   latestProgress == 1.0f &&
                   probeFfmpegAudioStream(flac, &exportedFormat, &error) &&
                   exportedFormat.sampleRate == 44100 &&
                   exportedFormat.channels == 1,
               "audio export must preserve a representable native format");

  const std::filesystem::path cancelledFlac =
      directory / "cancelled.flac";
  bool cancel = false;
  const bool cancelled = audio_export::exportToFlac(
      wave, cancelledFlac,
      [&](float, const std::string& phase) {
        if (phase == "Extracting lossless audio") cancel = true;
      },
      [&]() { return cancel; }, &error);
  ok &= expect(!cancelled && !std::filesystem::exists(cancelledFlac) &&
                   !hasTemporaryOutput(directory),
               "cancelled audio export must leave no partial artifact");

  const std::filesystem::path video = directory / "movie.mp4";
  std::ofstream(video, std::ios::binary).put('\0');
  const std::filesystem::path indexed =
      playback_video_transcript::transcriptPathForVideo(video);
  const std::vector<playback_video_transcript::Segment> segments = {
      {0, 1000000, "First sentence."},
      {1500000, 2500000, "Second sentence."}};
  ok &= expect(playback_video_transcript::writeIndexedTranscript(
                   indexed, segments,
                   playback_video_transcript::TranscriptPublishMode::CreateNew,
                   &error),
               "the indexed transcript fixture must be writable");
  const std::filesystem::path text =
      playback_video_transcript::uniqueTextExportPathForVideo(video);
  ok &= expect(playback_video_transcript::exportTranscriptText(
                   video, text, {}, []() { return false; }, &error),
               "plain-text transcript export must succeed");
  ok &= expect(
      text.filename() == "movie - transcript.txt" &&
          readFile(text) == "First sentence.\r\nSecond sentence.\r\n" &&
          playback_video_sidecars::classifyAutomaticSubtitleSidecar(
              video, text) ==
              playback_video_sidecars::AutomaticSubtitleKind::None,
      "text exports must remain readable documents rather than sidecars");

  std::error_code cleanupError;
  std::filesystem::remove_all(directory, cleanupError);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
