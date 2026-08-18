#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "browser_model.h"
#include "tracklist.h"

struct TrackBrowserContent {
  std::filesystem::path file;
  PathIdentity fileIdentity;
  std::vector<TrackEntry> tracks;
};

std::filesystem::path normalizeTrackBrowserPath(std::filesystem::path path);
bool listTracksForFile(const std::filesystem::path& path,
                       std::vector<TrackEntry>* tracks,
                       std::string* error);
std::shared_ptr<const TrackBrowserContent> prepareTrackBrowserContent(
    const std::filesystem::path& file);
bool isTrackBrowserActive(const BrowserState& state);
const TrackBrowserContent* trackBrowserContent(const BrowserState& state);
const TrackEntry* findTrackEntry(const BrowserState& state, int trackIndex);
