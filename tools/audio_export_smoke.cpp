#include "audio/audio_export.h"
#include "audio/ffmpegaudio.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: audio_export_smoke <media-file>\n";
    return 2;
  }

  const std::filesystem::path source = argv[1];
  std::string error;
  FfmpegAudioStreamFormat format;
  if (!probeFfmpegAudioStream(source, &format, &error)) {
    std::cerr << "Could not probe source audio: " << error << '\n';
    return 1;
  }

  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("radioify-audio-export-smoke-" + std::to_string(stamp));
  std::filesystem::create_directories(directory);
  const std::filesystem::path output = directory / "cancelled.flac";

  int cancellationChecks = 0;
  float latestProgress = 0.0f;
  const bool succeeded = audio_export::exportToFlac(
      source, output,
      [&](float progress, std::string) { latestProgress = progress; },
      [&]() { return ++cancellationChecks >= 3; }, &error);
  const bool cleanCancellation =
      !succeeded && cancellationChecks >= 3 &&
      !std::filesystem::exists(output);

  std::error_code cleanupError;
  std::filesystem::remove_all(directory, cleanupError);
  if (!cleanCancellation || cleanupError) {
    std::cerr << "Cancellation smoke failed: " << error << '\n';
    return 1;
  }

  std::cout << "Decoded " << format.sampleRate << " Hz, "
            << format.channels << " channel(s); cancelled cleanly at "
            << static_cast<int>(latestProgress * 100.0f) << "%\n";
  return EXIT_SUCCESS;
}
