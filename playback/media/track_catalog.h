#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "tracklist.h"

std::filesystem::path normalizePlaybackTrackPath(std::filesystem::path path);
bool listPlaybackTracks(const std::filesystem::path& path,
                        std::vector<TrackEntry>* tracks,
                        std::string* error);
const TrackEntry* findPlaybackTrack(const std::vector<TrackEntry>& tracks,
                                    int trackIndex);
