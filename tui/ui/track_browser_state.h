#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "tracklist.h"

#include "browser_model.h"

std::filesystem::path normalizeTrackBrowserPath(std::filesystem::path path);
bool listTracksForFile(const std::filesystem::path& path,
                       std::vector<TrackEntry>* tracks,
                       std::string* error);
bool loadTrackBrowserForFile(const std::filesystem::path& file);
void clearTrackBrowserState();
bool trackBrowserActive();
const std::filesystem::path& trackBrowserFile();
const std::vector<TrackEntry>& trackBrowserTracks();
bool isTrackBrowserActive(const BrowserState& state);
const TrackEntry* findTrackEntry(int trackIndex);
