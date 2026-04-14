#include "track_browser_state.h"

#include <utility>

#include "consoleinput.h"
#include "gmeaudio.h"
#include "gsfaudio.h"
#include "kssaudio.h"
#include "media_formats.h"
#include "psfaudio.h"
#include "vgmaudio.h"

namespace {

struct TrackBrowserState {
  bool active = false;
  std::filesystem::path file;
  std::vector<TrackEntry> tracks;
};

TrackBrowserState gTrackBrowser;

}  // namespace

std::filesystem::path normalizeTrackBrowserPath(std::filesystem::path path) {
  if (!path.has_parent_path()) {
    path = std::filesystem::path(".") / path;
  }
  return path;
}

bool listTracksForFile(const std::filesystem::path& path,
                       std::vector<TrackEntry>* tracks,
                       std::string* error) {
  if (isKssExt(path)) {
    return kssListTracks(path, tracks, error);
  }
  if (isGmeExt(path)) {
    return gmeListTracks(path, tracks, error);
  }
  if (isGsfExt(path)) {
    return gsfListTracks(path, tracks, error);
  }
  if (isVgmExt(path)) {
    return vgmListTracks(path, tracks, error);
  }
  if (isPsfExt(path)) {
    return psfListTracks(path, tracks, error);
  }
  return false;
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
  if (trackIndex < 0) return nullptr;
  if (trackIndex < static_cast<int>(gTrackBrowser.tracks.size())) {
    const auto& entry = gTrackBrowser.tracks[static_cast<size_t>(trackIndex)];
    if (entry.index == trackIndex) {
      return &entry;
    }
  }
  for (const auto& entry : gTrackBrowser.tracks) {
    if (entry.index == trackIndex) return &entry;
  }
  return nullptr;
}
