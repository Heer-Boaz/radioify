#include "browser_playback_source.h"

#include <utility>

#include "audio/media_formats.h"

namespace browser_playback_source {

std::optional<PlaybackTarget> targetFor(const BrowserEntry& entry) {
  if (const auto* track = entry.actionAs<browser_entry::PlayTrack>()) {
    return PlaybackTarget{entry.path, track->trackIndex};
  }
  if (entry.actionAs<browser_entry::OpenFile>() &&
      isSupportedMediaExt(entry.path)) {
    return PlaybackTarget{entry.path, -1};
  }
  return std::nullopt;
}

playback_queue::Source capture(const std::vector<BrowserEntry>& entries) {
  std::vector<PlaybackTarget> targets;
  targets.reserve(entries.size());
  for (const BrowserEntry& entry : entries) {
    std::optional<PlaybackTarget> target = targetFor(entry);
    if (target && !isSupportedImageExt(target->file)) {
      targets.push_back(std::move(*target));
    }
  }
  return playback_queue::sourceFromTargets(std::move(targets));
}

}  // namespace browser_playback_source
