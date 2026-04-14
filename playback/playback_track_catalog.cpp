#include "playback_track_catalog.h"

#include "gmeaudio.h"
#include "gsfaudio.h"
#include "kssaudio.h"
#include "media_formats.h"
#include "psfaudio.h"
#include "vgmaudio.h"

std::filesystem::path normalizePlaybackTrackPath(std::filesystem::path path) {
  if (!path.has_parent_path()) {
    path = std::filesystem::path(".") / path;
  }
  return path;
}

bool listPlaybackTracks(const std::filesystem::path& path,
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

const TrackEntry* findPlaybackTrack(const std::vector<TrackEntry>& tracks,
                                    int trackIndex) {
  if (trackIndex < 0) {
    return nullptr;
  }
  if (trackIndex < static_cast<int>(tracks.size())) {
    const auto& entry = tracks[static_cast<size_t>(trackIndex)];
    if (entry.index == trackIndex) {
      return &entry;
    }
  }
  for (const auto& entry : tracks) {
    if (entry.index == trackIndex) {
      return &entry;
    }
  }
  return nullptr;
}
