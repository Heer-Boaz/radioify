#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
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

void melodyOfflineStart(const std::filesystem::path& file, int trackIndex,
                       uint32_t sourceSampleRate, uint32_t channels,
                       uint64_t leadInFrames,
                       const KssPlaybackOptions& kssOptions,
                       const NsfPlaybackOptions& nsfOptions,
                       const VgmPlaybackOptions& vgmOptions,
                       const std::unordered_map<uint32_t, VgmDeviceOptions>&
                           vgmDeviceOverrides);
void melodyOfflineStop();
MelodyOfflineFrame melodyOfflineGetFrame(double timeSec);
MelodyOfflineAnalysisState melodyOfflineGetState();

bool melodyOfflineAnalyzeToFile(
    const std::filesystem::path& file, int trackIndex, uint32_t sourceSampleRate,
    uint32_t channels, uint64_t leadInFrames,
    const KssPlaybackOptions& kssOptions,
    const NsfPlaybackOptions& nsfOptions,
    const VgmPlaybackOptions& vgmOptions,
    const std::unordered_map<uint32_t, VgmDeviceOptions>& vgmDeviceOverrides,
    const std::filesystem::path& outputFile,
    const std::function<void(float)>& progressCallback, std::string* error);
