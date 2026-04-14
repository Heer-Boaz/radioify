#pragma once

#include <string>

#include "playback_media_metadata_catalog.h"

struct AsciiArt;

bool resolvePlaybackMediaArtworkAscii(const PlaybackMediaDisplayRequest& request,
                                      const PlaybackMediaDisplayInfo& info,
                                      int maxWidth,
                                      int maxHeight,
                                      AsciiArt* out,
                                      std::string* error);

bool resolvePlaybackMediaArtworkBitmap(
    const PlaybackMediaDisplayRequest& request,
    const PlaybackMediaDisplayInfo& info,
    PlaybackMediaArtwork* out,
    std::string* error);
