#pragma once

#include <filesystem>

#include "playback/media_action_catalog.h"

namespace playback_media_processing {

struct SourceState {
  bool backgroundTaskRunning = false;
  bool subtitleGenerationRunning = false;
  bool audioSeparationAvailable = false;
  bool audioSeparationRunning = false;
  bool separatedAudioExists = false;
};

// Application-facing service contract consumed by both playback surfaces.
// It projects one coherent source snapshot instead of exposing worker-specific
// callbacks and synchronization details.
class Service {
 public:
  virtual ~Service() = default;

  virtual bool busy() const = 0;
  virtual SourceState sourceStateFor(
      const std::filesystem::path& sourceFile) const = 0;
  virtual bool requestSubtitles(
      const std::filesystem::path& sourceFile) = 0;
  virtual bool requestSubtitleCancellation() = 0;
  virtual bool requestAudioSeparation(
      const std::filesystem::path& sourceFile) = 0;
  virtual bool requestAudioSeparationCancellation() = 0;
};

// Small nullable facade copied into playback sessions. The bound service is
// borrowed and must outlive every Actions copy.
class Actions {
 public:
  Actions() = default;
  explicit Actions(Service& service) : service_(&service) {}

  bool busy() const;
  bool requestSubtitles(const std::filesystem::path& sourceFile) const;
  bool requestSubtitleCancellation() const;
  bool requestAudioSeparation(const std::filesystem::path& sourceFile) const;
  bool requestAudioSeparationCancellation() const;
  void applySourceState(const std::filesystem::path& sourceFile,
                        playback_media_actions::Context* context) const;

 private:
  Service* service_ = nullptr;
};

}  // namespace playback_media_processing
