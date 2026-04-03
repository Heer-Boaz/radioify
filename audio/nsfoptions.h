#pragma once

#include <cstdint>

enum class NsfEqPreset : uint8_t {
  Nes = 0,
  Famicom = 1,
};

enum class NsfStereoDepth : uint8_t {
  Off = 0,
  Low = 1,
  High = 2,
};

enum class NsfTempoMode : uint8_t {
  Normal = 0,
  Pal50 = 1,
};

struct NsfPlaybackOptions {
  NsfEqPreset eqPreset = NsfEqPreset::Nes;
  NsfStereoDepth stereoDepth = NsfStereoDepth::Off;
  bool ignoreSilence = false;
  NsfTempoMode tempoMode = NsfTempoMode::Normal;
};

enum class NsfOptionId : uint8_t {
  EqPreset = 0,
  StereoDepth = 1,
  IgnoreSilence = 2,
  TempoMode = 3,
};
