#ifndef PSFAUDIO_H
#define PSFAUDIO_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "tracklist.h"

class PsfAudioDecoder {
 public:
  PsfAudioDecoder();
  ~PsfAudioDecoder();

  bool init(const std::filesystem::path& path, uint32_t channels,
            uint32_t sampleRate, std::string* error, int trackIndex = 0);
  void uninit();
  bool readFrames(float* out, uint32_t frameCount, uint64_t* framesRead);
  bool seekToFrame(uint64_t frame);
  bool getTotalFrames(uint64_t* outFrames) const;
  bool active() const { return impl_ != nullptr; }

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

bool psfListTracks(const std::filesystem::path& path,
                   std::vector<TrackEntry>* out,
                   std::string* error);

#endif
