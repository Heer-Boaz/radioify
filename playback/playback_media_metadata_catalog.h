#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct PlaybackMediaArtwork {
  enum class Kind : uint8_t {
    None,
    Bgra32,
  };

  Kind kind = Kind::None;
  std::vector<uint8_t> bytes;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct PlaybackMediaDisplayRequest {
  bool isVideo = false;
  std::filesystem::path file;
  int trackIndex = -1;
};

struct PlaybackMediaDisplayInfo {
  std::string title;
  std::string subtitle;
  std::string artist;
  std::string albumTitle;
  std::string albumArtist;
  uint32_t trackNumber = 0;
  PlaybackMediaArtwork artwork;
};

bool resolvePlaybackMediaDisplayInfo(const PlaybackMediaDisplayRequest& request,
                                     PlaybackMediaDisplayInfo* out,
                                     std::string* error);
