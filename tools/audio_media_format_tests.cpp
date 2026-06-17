#include "media_formats.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

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

int main() {
  bool ok = true;

  const std::filesystem::path flac = "chapter3-1.flac";
  ok &= expect(isSupportedAudioExt(flac), "FLAC must remain supported audio");
  ok &= expect(isFlacExt(flac), "FLAC must keep its explicit format marker");
  ok &= expect(isFfmpegAudioExt(flac),
               "FLAC must keep the FFmpeg backend route");
  ok &= expect(!isMiniaudioExt(flac),
               "FLAC playback must not use the miniaudio backend route");

  const std::filesystem::path wma = "broadcast.WMA";
  ok &= expect(isSupportedAudioExt(wma), "WMA must be supported audio");
  ok &= expect(isFfmpegAudioExt(wma),
               "WMA must use the FFmpeg audio backend route");
  ok &= expect(!isMiniaudioExt(wma),
               "WMA must not use the miniaudio backend route");
  ok &= expect(!isSupportedAudioExt("camera.wmv"),
               "WMV must remain a video extension, not an audio alias");
  ok &= expect(supportedAudioExtensionsText().find(".wma") != std::string::npos,
               "Supported audio extension text must mention WMA");

  ok &= expect(isMiniaudioExt("track.wav"),
               "WAV must keep the miniaudio backend route");
  ok &= expect(isMiniaudioExt("track.mp3"),
               "MP3 must keep the miniaudio backend route");

  return ok ? 0 : 1;
}
