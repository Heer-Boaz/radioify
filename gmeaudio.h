#ifndef GMEAUDIO_H
#define GMEAUDIO_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "tracklist.h"

extern "C" {
#include <gme/gme.h>
}

class GmeAudioDecoder {
 public:
  GmeAudioDecoder();
  ~GmeAudioDecoder();

  bool init(const std::filesystem::path& path, uint32_t channels,
            uint32_t sampleRate, std::string* error, int trackIndex = 0);
  void uninit();
  bool readFrames(float* out, uint32_t frameCount, uint64_t* framesRead);
  bool seekToFrame(uint64_t frame);
  bool getTotalFrames(uint64_t* outFrames) const;
  bool active() const { return emu_ != nullptr; }
  const std::string& warning() const { return warning_; }

 private:
  Music_Emu* emu_ = nullptr;
  uint32_t channels_ = 0;
  uint32_t sampleRate_ = 0;
  uint64_t totalFrames_ = 0;
  uint64_t framePos_ = 0;
  bool atEnd_ = false;
  std::vector<int16_t> buffer_;
  std::string warning_;
};

bool gmeListTracks(const std::filesystem::path& path,
                   std::vector<TrackEntry>* out,
                   std::string* error);

#endif
