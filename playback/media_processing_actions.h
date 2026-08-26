#pragma once

#include <filesystem>
#include <functional>

#include "playback/media_action_catalog.h"

namespace playback_media_processing {

// Boundary between playback surfaces and the application-owned media task
// coordinator. Browser and player consume the same source-state projection and
// commands without depending on concrete worker types.
struct Actions {
  std::function<bool()> anyRunning;
  std::function<bool(const std::filesystem::path&)> startSubtitles;
  std::function<bool(const std::filesystem::path&)> subtitlesRunningFor;
  std::function<bool()> cancelSubtitles;
  std::function<bool(const std::filesystem::path&)> canSeparateAudio;
  std::function<bool(const std::filesystem::path&)> startAudioSeparation;
  std::function<bool(const std::filesystem::path&)> audioSeparationRunningFor;
  std::function<bool(const std::filesystem::path&)> hasSeparatedAudio;
  std::function<bool()> cancelAudioSeparation;

  bool busy() const;
  bool requestSubtitles(const std::filesystem::path& sourceFile) const;
  bool requestSubtitleCancellation() const;
  bool requestAudioSeparation(const std::filesystem::path& sourceFile) const;
  bool requestAudioSeparationCancellation() const;
  void applySourceState(const std::filesystem::path& sourceFile,
                        playback_media_actions::Context* context) const;
};

}  // namespace playback_media_processing
