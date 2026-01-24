#pragma once

#include <cstdint>

enum class KssSccType : uint8_t {
  Auto = 0,
  Standard = 1,
  Enhanced = 2,
};

enum class KssQuality : uint8_t {
  Auto = 0,
  Low = 1,
  High = 2,
};

struct KssPlaybackOptions {
  bool force50Hz = false;
  KssSccType sccType = KssSccType::Auto;
  KssQuality psgQuality = KssQuality::Auto;
  KssQuality sccQuality = KssQuality::Auto;
  bool opllStereo = false;
  bool mutePsg = false;
  bool muteScc = false;
  bool muteOpll = false;
};

enum class KssOptionId : uint8_t {
  Force50Hz = 0,
  SccType = 1,
  PsgQuality = 2,
  SccQuality = 3,
  OpllStereo = 4,
  MutePsg = 5,
  MuteScc = 6,
  MuteOpll = 7,
};
