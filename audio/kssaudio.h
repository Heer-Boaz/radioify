#ifndef KSSAUDIO_H
#define KSSAUDIO_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "kssoptions.h"
#include "tracklist.h"

extern "C" {
#include <kssplay.h>
}

class KssAudioDecoder {
 public:
  KssAudioDecoder();
  ~KssAudioDecoder();

  bool init(const std::filesystem::path& path, uint32_t channels,
            uint32_t sampleRate, std::string* error, int trackIndex = 0,
            bool force50Hz = false);
  bool init(const std::filesystem::path& path, uint32_t channels,
            uint32_t sampleRate, std::string* error, int trackIndex,
            const KssPlaybackOptions& options);
  void uninit();
  bool readFrames(float* out, uint32_t frameCount, uint64_t* framesRead);
  bool seekToFrame(uint64_t frame);
  bool getTotalFrames(uint64_t* outFrames) const;
  bool active() const { return kssplay_ != nullptr; }
  bool readDeviceRegs(KSS_DEVICE device, std::vector<uint8_t>* out) const;

 private:
  KSS* kss_ = nullptr;
  KSSPLAY* kssplay_ = nullptr;
  uint32_t channels_ = 0;
  uint32_t sampleRate_ = 0;
  uint64_t totalFrames_ = 0;
  uint64_t framePos_ = 0;
  bool atEnd_ = false;
  bool fadeArmed_ = false;
  uint64_t fadeStartFrame_ = 0;
  uint32_t fadeDurationMs_ = 0;
  std::vector<int16_t> buffer_;
  int trackBase_ = 0;
  int trackCount_ = 0;
  int trackIndex_ = 0;
  KssPlaybackOptions options_{};
};

bool kssListTracks(const std::filesystem::path& path,
                   std::vector<TrackEntry>* out,
                   std::string* error);

#endif
