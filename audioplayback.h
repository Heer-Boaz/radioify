#pragma once

#include <cstdint>
#include <filesystem>

struct AudioPerfStats {
  uint64_t callbacks = 0;
  uint64_t framesRequested = 0;
  uint64_t framesRead = 0;
  uint64_t shortReads = 0;
  uint64_t silentFrames = 0;
  uint32_t lastCallbackFrames = 0;
  uint32_t lastFramesRead = 0;
  uint32_t periodFrames = 0;
  uint32_t periods = 0;
  uint32_t bufferFrames = 0;
  uint32_t sampleRate = 0;
  uint32_t channels = 0;
  bool usingFfmpeg = false;
};

struct AudioPlaybackConfig {
  bool enableAudio = true;
  bool enableRadio = false;
  bool mono = true;
  bool dry = false;
  int bwHz = 5500;
  double noise = 0.012;
};

void audioInit(const AudioPlaybackConfig& config);
void audioShutdown();

bool audioIsEnabled();
bool audioIsReady();

bool audioStartFile(const std::filesystem::path& file);
bool audioStartFileAt(const std::filesystem::path& file, double startSec);
void audioStop();

std::filesystem::path audioGetNowPlaying();

double audioGetTimeSec();
double audioGetTotalSec();
bool audioIsSeeking();
double audioGetSeekTargetSec();

bool audioIsPrimed();
bool audioIsPaused();
bool audioIsFinished();
bool audioIsRadioEnabled();
bool audioIsHolding();

AudioPerfStats audioGetPerfStats();
int audioGetDebugTap();
int audioGetDebugTapMax();
void audioCycleDebugTap();
void audioSetDebugTap(int tap);

void audioTogglePause();
void audioSeekBy(int direction);
void audioSeekToRatio(double ratio);
void audioSeekToSec(double sec);
void audioToggleRadio();
void audioSetHold(bool hold);
