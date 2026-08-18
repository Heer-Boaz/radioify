#include "track_browser_state.h"

#include <utility>

#include "playback/media/track_catalog.h"

std::filesystem::path normalizeTrackBrowserPath(std::filesystem::path path) {
  return normalizePlaybackTrackPath(std::move(path));
}

bool listTracksForFile(const std::filesystem::path& path,
                       std::vector<TrackEntry>* tracks,
                       std::string* error) {
  return listPlaybackTracks(path, tracks, error);
}

std::shared_ptr<const TrackBrowserContent> prepareTrackBrowserContent(
    const std::filesystem::path& file) {
  std::filesystem::path trackPath = normalizeTrackBrowserPath(file);
  std::vector<TrackEntry> tracks;
  std::string error;
  if (!listTracksForFile(trackPath, &tracks, &error) || tracks.size() <= 1) {
    return {};
  }
  auto content = std::make_shared<TrackBrowserContent>();
  content->file = std::move(trackPath);
  content->fileIdentity = makePathIdentity(content->file);
  content->tracks = std::move(tracks);
  return content;
}

bool isTrackBrowserActive(const BrowserState& state) {
  return state.location.kind == BrowserLocationKind::TrackBrowser;
}

const TrackBrowserContent* trackBrowserContent(const BrowserState& state) {
  const auto* content =
      std::get_if<std::shared_ptr<const TrackBrowserContent>>(&state.content);
  return content && *content ? content->get() : nullptr;
}

const TrackEntry* findTrackEntry(const BrowserState& state, int trackIndex) {
  const TrackBrowserContent* content = trackBrowserContent(state);
  return content ? findPlaybackTrack(content->tracks, trackIndex) : nullptr;
}
