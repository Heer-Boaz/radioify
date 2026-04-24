#include "track_browser_state.h"

#include <utility>

#include "consoleinput.h"
#include "playback/media/track_catalog.h"

namespace {

struct TrackBrowserState {
  bool active = false;
  std::filesystem::path file;
  std::vector<TrackEntry> tracks;
};

TrackBrowserState gTrackBrowser;

}  // namespace

std::filesystem::path normalizeTrackBrowserPath(std::filesystem::path path) {
  return normalizePlaybackTrackPath(std::move(path));
}

bool listTracksForFile(const std::filesystem::path& path,
                       std::vector<TrackEntry>* tracks,
                       std::string* error) {
  return listPlaybackTracks(path, tracks, error);
}

bool loadTrackBrowserForFile(const std::filesystem::path& file) {
  std::filesystem::path trackPath = normalizeTrackBrowserPath(file);
  std::vector<TrackEntry> tracks;
  std::string error;
  bool listed = listTracksForFile(trackPath, &tracks, &error);
  if (!listed) {
    gTrackBrowser = TrackBrowserState{};
    return false;
  }
  gTrackBrowser.file = trackPath;
  gTrackBrowser.tracks = std::move(tracks);
  gTrackBrowser.active = gTrackBrowser.tracks.size() > 1;
  return gTrackBrowser.active;
}

void clearTrackBrowserState() { gTrackBrowser = TrackBrowserState{}; }

bool trackBrowserActive() { return gTrackBrowser.active; }

const std::filesystem::path& trackBrowserFile() { return gTrackBrowser.file; }

const std::vector<TrackEntry>& trackBrowserTracks() {
  return gTrackBrowser.tracks;
}

bool isTrackBrowserActive(const BrowserState& state) {
  return gTrackBrowser.active && state.dir == gTrackBrowser.file;
}

const TrackEntry* findTrackEntry(int trackIndex) {
  return findPlaybackTrack(gTrackBrowser.tracks, trackIndex);
}
