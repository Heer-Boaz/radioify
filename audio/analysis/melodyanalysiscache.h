#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "kssoptions.h"
#include "nsfoptions.h"
#include "vgmoptions.h"

struct MelodyOfflineFrame {
  float frequencyHz = 0.0f;
  float confidence = 0.0f;
  int midiNote = -1;
};

struct MelodyOfflineAnalysisState {
  bool ready = false;
  bool running = false;
  float progress = 0.0f;
  size_t frameCount = 0;
  std::string error;
};

using MelodyOutputCommitStarted = std::function<bool()>;

// Session-scoped cache for the analysis that follows active playback. Each
// audio runtime owns one instance, including its worker lifetime and results.
class MelodyOfflineCache {
 public:
  MelodyOfflineCache();
  ~MelodyOfflineCache();

  MelodyOfflineCache(const MelodyOfflineCache&) = delete;
  MelodyOfflineCache& operator=(const MelodyOfflineCache&) = delete;
  MelodyOfflineCache(MelodyOfflineCache&&) = delete;
  MelodyOfflineCache& operator=(MelodyOfflineCache&&) = delete;

  void start(const std::filesystem::path& file, int trackIndex,
             uint32_t sourceSampleRate, uint32_t channels,
             uint64_t leadInFrames, const KssPlaybackOptions& kssOptions,
             const NsfPlaybackOptions& nsfOptions,
             const VgmPlaybackOptions& vgmOptions,
             const std::unordered_map<uint32_t, VgmDeviceOptions>&
                 vgmDeviceOverrides);
  void stop();
  MelodyOfflineFrame frameAt(double timeSec) const;
  MelodyOfflineAnalysisState state() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

bool melodyOfflineCanAnalyzeFile(const std::filesystem::path& file);

bool melodyOfflineAnalyzeToFile(
    const std::filesystem::path& file, int trackIndex, uint32_t sourceSampleRate,
    uint32_t channels, uint64_t leadInFrames,
    const KssPlaybackOptions& kssOptions,
    const NsfPlaybackOptions& nsfOptions,
    const VgmPlaybackOptions& vgmOptions,
    const std::unordered_map<uint32_t, VgmDeviceOptions>& vgmDeviceOverrides,
    const std::filesystem::path& outputFile,
    const std::function<void(float)>& progressCallback,
    const std::function<bool()>& cancellationRequested, std::string* error,
    const MelodyOutputCommitStarted& outputCommitStarted = {});
