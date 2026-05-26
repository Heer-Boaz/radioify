#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <thread>

#include "kssoptions.h"

extern "C" {
#include <kssplay.h>
}

constexpr uint32_t kMsxClockHz = 3579545u;
constexpr float kAuditionGain = 0.28f;

enum class AuditionKind {
  Psg,
  SccWave,
};

struct AuditionTone {
  AuditionKind kind = AuditionKind::SccWave;
  PSG* psg = nullptr;
  SCC* scc = nullptr;
  float gain = 0.0f;
};

struct AuditionState {
  std::atomic<bool> active{false};
  std::atomic<bool> stop{false};
  std::thread worker;
  KssInstrumentDevice device = KssInstrumentDevice::None;
  uint32_t hash = 0;
  std::filesystem::path resumeFile;
  int resumeTrackIndex = 0;
  uint64_t resumeFrame = 0;
  bool resumePaused = false;
  bool resumeValid = false;
};

void startAuditionWorker(AuditionTone tone);
void stopAuditionWorker();
