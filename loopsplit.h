#ifndef LOOPSPLIT_H
#define LOOPSPLIT_H

#include <cstdint>
#include <filesystem>
#include <string>

struct LoopSplitConfig {
  uint32_t sampleRate = 48000;
  uint32_t channels = 2;
  float minLoopSeconds = 1.0f;
  float maxLoopSeconds = 180.0f;
  uint32_t analysisHop = 256;
  uint32_t coarseWindowSeconds = 2;
  uint32_t refineWindowSeconds = 1;
  float minConfidence = 0.55f;
};

struct LoopSplitResult {
  bool hasLoop = false;
  bool hasStinger = false;
  uint64_t loopStartFrame = 0;
  uint64_t loopFrameCount = 0;
  float confidence = 0.0f;
  uint64_t totalFrames = 0;
};

bool splitAudioIntoLoopFiles(const std::filesystem::path& inputFile,
                            const std::filesystem::path& stingerOutput,
                            const std::filesystem::path& loopOutput,
                            const LoopSplitConfig& config,
                            LoopSplitResult* result,
                            std::string* error);

#endif  // LOOPSPLIT_H
