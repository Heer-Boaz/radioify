#include "playback_queue.h"

#include <utility>

namespace playback_queue {

Source fromTargets(std::vector<PlaybackTarget> targets) {
  return Source(std::move(targets));
}

Source fromFiles(const std::vector<std::filesystem::path>& files) {
  std::vector<PlaybackTarget> targets;
  targets.reserve(files.size());
  for (const std::filesystem::path& file : files) {
    if (!file.empty()) {
      targets.push_back({file, -1});
    }
  }
  return fromTargets(std::move(targets));
}

Source single(const PlaybackTarget& target) {
  std::vector<PlaybackTarget> targets;
  if (!target.file.empty()) {
    targets.push_back(target);
  }
  return fromTargets(std::move(targets));
}

Queue::Queue(ResolvePathTarget resolvePathTarget)
    : resolvePathTarget_(std::move(resolvePathTarget)) {}

bool Queue::matchesTarget(const Entry& entry, const PlaybackTarget& target,
                          const PathIdentity& targetIdentity) {
  if (target.file.empty() || entry.fileIdentity != targetIdentity) {
    return false;
  }

  // Directory/file sources represent a container as one queue item. Once that
  // item resolves to a concrete internal track it still identifies the same
  // queue position. Track-browser sources carry exact track identities.
  return entry.target.trackIndex < 0 ||
         entry.target.trackIndex == target.trackIndex;
}

std::optional<std::size_t> Queue::findTarget(const std::vector<Entry>& entries,
                                             const PlaybackTarget& target) {
  if (target.file.empty()) {
    return std::nullopt;
  }

  const PathIdentity targetIdentity = makePathIdentity(target.file);
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (matchesTarget(entries[index], target, targetIdentity)) {
      return index;
    }
  }
  return std::nullopt;
}

bool Queue::activate(Source source, const PlaybackTarget& current) {
  std::vector<Entry> candidate;
  candidate.reserve(source.targets_.size());
  for (PlaybackTarget& target : source.targets_) {
    if (target.file.empty()) {
      continue;
    }
    Entry entry;
    entry.target = std::move(target);
    entry.fileIdentity = makePathIdentity(entry.target.file);
    candidate.push_back(std::move(entry));
  }

  const std::optional<std::size_t> currentIndex =
      findTarget(candidate, current);
  if (!currentIndex) {
    return false;
  }

  entries_ = std::move(candidate);
  currentIndex_ = currentIndex;
  return true;
}

bool Queue::select(const PlaybackTarget& target) {
  const std::optional<std::size_t> index = findTarget(entries_, target);
  if (!index) {
    return false;
  }
  currentIndex_ = index;
  return true;
}

std::optional<PlaybackTarget> Queue::adjacent(Direction direction) const {
  if (!currentIndex_) {
    return std::nullopt;
  }

  std::size_t index = *currentIndex_;
  while (true) {
    if (direction == Direction::Previous) {
      if (index == 0) {
        return std::nullopt;
      }
      --index;
    } else {
      if (index + 1 >= entries_.size()) {
        return std::nullopt;
      }
      ++index;
    }

    const PlaybackTarget& candidate = entries_[index].target;
    if (candidate.trackIndex >= 0) {
      return candidate;
    }
    if (resolvePathTarget_) {
      if (std::optional<PlaybackTarget> resolved =
              resolvePathTarget_(candidate.file)) {
        return resolved;
      }
    }
  }
}

}  // namespace playback_queue
