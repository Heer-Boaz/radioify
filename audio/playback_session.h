#pragma once

#include <filesystem>

#include "audio/playback_snapshot.h"

namespace audio_playback {

// Application-facing audio-session protocol. Shell orchestration depends on
// transport and activation semantics, not on decoder/stream implementation.
class Session {
 public:
  virtual ~Session() = default;

  virtual bool startFile(const std::filesystem::path& file,
                         int trackIndex) = 0;
  virtual void stop() = 0;
  virtual AudioPlaybackSnapshot snapshot() const = 0;
  virtual void play() = 0;
  virtual void pause() = 0;
  virtual void togglePause() = 0;
  virtual void seekToRatio(double ratio) = 0;
};

}  // namespace audio_playback
