#ifndef VGMAUDIO_H
#define VGMAUDIO_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "tracklist.h"
#include "vgmoptions.h"

extern "C" {
#include <utils/DataLoader.h>
}

class PlayerA;

class VgmAudioDecoder {
 public:
  VgmAudioDecoder();
  ~VgmAudioDecoder();

  bool init(const std::filesystem::path& path, uint32_t channels,
            uint32_t sampleRate, std::string* error);
  void uninit();
  bool readFrames(float* out, uint32_t frameCount, uint64_t* framesRead);
  bool seekToFrame(uint64_t frame);
  bool getTotalFrames(uint64_t* outFrames) const;
  bool active() const { return active_; }
  void applyOptions(const VgmPlaybackOptions& options);
  const std::string& warning() const { return warning_; }

 private:
  bool initPlayer(std::string* error);
  bool loadFile(const std::filesystem::path& path, std::string* error);
  void destroyPlayer();
  bool resetPlayback(std::string* error);
  void updateBaseFrames();
  void updateTotalFrames();

  std::filesystem::path file_;
  PlayerA* player_ = nullptr;
  DATA_LOADER* loader_ = nullptr;
  uint32_t channels_ = 0;
  uint32_t sampleRate_ = 0;
  uint32_t outputChannels_ = 2;
  uint64_t baseFrames_ = 0;
  uint64_t totalFrames_ = 0;
  uint64_t framePos_ = 0;
  bool atEnd_ = false;
  bool active_ = false;
  double playbackSpeed_ = 1.0;
  std::vector<int16_t> buffer_;
  std::string warning_;
  VgmPlaybackOptions options_{};
};

bool vgmListTracks(const std::filesystem::path& path,
                   std::vector<TrackEntry>* out,
                   std::string* error);

#endif
