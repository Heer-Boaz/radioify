#pragma once

#include <filesystem>

struct PlaybackTarget {
  std::filesystem::path file;
  int trackIndex = -1;
};
