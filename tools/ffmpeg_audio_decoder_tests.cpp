#include "ffmpegaudio.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

void die(const std::string& message) {
  std::cerr << "ERROR: " << message << '\n';
  std::exit(1);
}

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "FAIL: " << message << '\n';
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: ffmpeg_audio_decoder_tests <flac-file>\n";
    return 2;
  }
  const std::filesystem::path flac = argv[1];

  std::string error;
  FfmpegAudioDecoder decoder;
  bool ok = true;
  ok &= expect(decoder.init(flac, 2, 48000, &error),
               "FFmpeg decoder must open FLAC fixture");
  if (!ok) {
    std::cerr << "init error: " << error << '\n';
    return 1;
  }

  uint64_t totalFrames = 0;
  ok &= expect(decoder.getTotalFrames(&totalFrames),
               "FLAC fixture must report total frames");
  ok &= expect(totalFrames > 48000,
               "FLAC fixture duration must be longer than one second");

  const uint64_t seekFrame = 48000;
  ok &= expect(decoder.seekToFrame(seekFrame),
               "FFmpeg decoder must seek within FLAC fixture");

  std::vector<float> samples(4096 * 2);
  uint64_t framesRead = 0;
  ok &= expect(decoder.readFrames(samples.data(), 4096, &framesRead),
               "FFmpeg decoder must read after FLAC seek");
  ok &= expect(framesRead > 0, "FLAC seek must produce decoded frames");

  return ok ? 0 : 1;
}
