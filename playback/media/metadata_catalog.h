#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "media_artwork_sidecar.h"
#include "playback/target.h"

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
  explicit PlaybackMediaDisplayRequest(PlaybackTarget playbackTarget,
                                       bool video = false)
      : target(std::move(playbackTarget)), isVideo(video) {}

  const std::filesystem::path& file() const {
    return playbackTargetFile(target);
  }

  std::optional<int> trackIndex() const {
    return playbackTargetTrackIndex(target);
  }

  PlaybackTarget target;
  bool isVideo = false;
};

struct PlaybackMediaDisplayResolveOptions {
  bool includeArtwork = true;
  MediaArtworkSidecarPolicy artworkSidecarPolicy =
      MediaArtworkSidecarPolicy::IncludeGenericDirectoryFallback;
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
                                     const PlaybackMediaDisplayResolveOptions& options,
                                     PlaybackMediaDisplayInfo* out,
                                     std::string* error);
