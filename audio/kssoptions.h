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

enum class KssPsgType : uint8_t {
  Auto = 0,
  Ay = 1,
  Ym = 2,
};

enum class KssOpllType : uint8_t {
  Ym2413 = 0,
  Vrc7 = 1,
  Ymf281b = 2,
};

enum class KssInstrumentDevice : uint8_t {
  None = 0,
  Psg = 1,
  Scc = 2,
  Opll = 3,
};

struct KssPlaybackOptions {
  bool force50Hz = false;
  KssSccType sccType = KssSccType::Auto;
  KssPsgType psgType = KssPsgType::Auto;
  KssOpllType opllType = KssOpllType::Ym2413;
  KssQuality psgQuality = KssQuality::Auto;
  KssQuality sccQuality = KssQuality::Auto;
  bool opllStereo = false;
  bool mutePsg = false;
  bool muteScc = false;
  bool muteOpll = false;
  KssInstrumentDevice instrumentDevice = KssInstrumentDevice::None;
  int instrumentChannel = -1;
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
  PsgType = 8,
  OpllType = 9,
};
