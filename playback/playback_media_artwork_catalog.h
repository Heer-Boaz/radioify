#pragma once

#include <string>

#include "media_artwork_sidecar.h"
#include "playback_media_metadata_catalog.h"

struct AsciiArt;

bool resolvePlaybackMediaArtworkAscii(const PlaybackMediaDisplayRequest& request,
                                      const PlaybackMediaDisplayInfo& info,
                                      MediaArtworkSidecarPolicy sidecarPolicy,
                                      int maxWidth,
                                      int maxHeight,
                                      AsciiArt* out,
                                      std::string* error);

bool resolvePlaybackMediaArtworkAscii(const PlaybackMediaDisplayRequest& request,
                                      MediaArtworkSidecarPolicy sidecarPolicy,
                                      int maxWidth,
                                      int maxHeight,
                                      AsciiArt* out,
                                      std::string* error);

bool resolvePlaybackMediaArtworkBitmap(
    const PlaybackMediaDisplayRequest& request,
    const PlaybackMediaDisplayInfo& info,
    MediaArtworkSidecarPolicy sidecarPolicy,
    PlaybackMediaArtwork* out,
    std::string* error);
