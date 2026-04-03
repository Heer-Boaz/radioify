#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class VgmPlaybackHz : uint8_t {
  Auto = 0,
  Hz60 = 1,
  Hz50 = 2,
};

enum class VgmPhaseInvert : uint8_t {
  Off = 0,
  Left = 1,
  Right = 2,
  Both = 3,
};

struct VgmPlaybackOptions {
  VgmPlaybackHz playbackHz = VgmPlaybackHz::Auto;
  int loopCount = 2;
  int fadeMs = 0;
  int endSilenceMs = 0;
  bool hardStopOld = false;
  bool ignoreVolGain = false;
  int masterVolumeStep = 3;
  int speedStep = 2;
  VgmPhaseInvert phaseInvert = VgmPhaseInvert::Off;
};

enum class VgmOptionId : uint8_t {
  PlaybackHz = 0,
  Speed = 1,
  LoopCount = 2,
  FadeLength = 3,
  EndSilence = 4,
  HardStopOld = 5,
  IgnoreVolGain = 6,
  MasterVolume = 7,
  PhaseInvert = 8,
};

enum class VgmDeviceOptionId : uint8_t {
  Mute = 0,
  Core = 1,
  Resampler = 2,
  SampleRateMode = 3,
  SampleRate = 4,
};

struct VgmMetadataEntry {
  std::string key;
  std::string value;
};

struct VgmDeviceInfo {
  uint32_t id = 0;
  std::string name;
  uint16_t instance = 0;
  uint16_t channelCount = 0;
  std::vector<uint32_t> coreIds;
  std::vector<std::string> coreNames;
};

struct VgmDeviceOptions {
  uint32_t coreId = 0;
  uint8_t resamplerMode = 0;
  uint8_t sampleRateMode = 0;
  uint32_t sampleRate = 0;
  bool muted = false;
};
