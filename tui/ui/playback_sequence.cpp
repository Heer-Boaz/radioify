#include "playback_sequence.h"

#include <utility>

bool PlaybackSequence::matchesTarget(const Entry& entry,
                                     const PlaybackTarget& target,
                                     const PathIdentity& targetIdentity) {
  if (target.file.empty() || entry.fileIdentity != targetIdentity) {
    return false;
  }

  // A directory sequence stores one item for a track container. Once playback
  // resolves that container to a concrete internal track, it must still select
  // the same sequence item. A virtual track-browser sequence stores exact
  // track indices and therefore requires an exact match.
  return entry.target.trackIndex < 0 ||
         entry.target.trackIndex == target.trackIndex;
}

void PlaybackSequence::replace(std::vector<PlaybackTarget> targets,
                               const PlaybackTarget& current) {
  entries_.clear();
  currentIndex_.reset();
  entries_.reserve(targets.size());
  for (PlaybackTarget& target : targets) {
    if (target.file.empty()) {
      continue;
    }
    Entry entry;
    entry.target = std::move(target);
    entry.fileIdentity = makePathIdentity(entry.target.file);
    entries_.push_back(std::move(entry));
  }

  if (select(current) || current.file.empty()) {
    return;
  }

  // The current item is an invariant of a playback sequence. A direct route
  // can legitimately be absent from the supplied source snapshot; in that
  // case it owns a single-item sequence instead of inheriting unrelated
  // context.
  replaceWithSingle(current);
}

void PlaybackSequence::replaceWithSingle(const PlaybackTarget& current) {
  entries_.clear();
  currentIndex_.reset();
  if (current.file.empty()) {
    return;
  }

  entries_.push_back({current, makePathIdentity(current.file)});
  currentIndex_ = 0;
}

void PlaybackSequence::clear() {
  entries_.clear();
  currentIndex_.reset();
}

bool PlaybackSequence::select(const PlaybackTarget& target) {
  if (target.file.empty()) {
    return false;
  }

  const PathIdentity targetIdentity = makePathIdentity(target.file);
  for (std::size_t index = 0; index < entries_.size(); ++index) {
    if (matchesTarget(entries_[index], target, targetIdentity)) {
      currentIndex_ = index;
      return true;
    }
  }
  return false;
}

std::optional<PlaybackTarget>
PlaybackSequence::adjacent(int direction, std::size_t distance) const {
  if (direction == 0 || distance == 0 || !currentIndex_) {
    return std::nullopt;
  }

  std::size_t index = *currentIndex_;
  if (direction < 0) {
    if (distance > index) {
      return std::nullopt;
    }
    index -= distance;
  } else {
    if (distance > entries_.size() - index - 1) {
      return std::nullopt;
    }
    index += distance;
  }
  return entries_[index].target;
}
