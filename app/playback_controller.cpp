#include "app/playback_controller.h"

#include <utility>

namespace playback_controller {

Source sourceFromTargets(std::vector<PlaybackTarget> targets) {
  return Source(std::move(targets));
}

Source sourceFromFiles(const std::vector<std::filesystem::path>& files) {
  std::vector<PlaybackTarget> targets;
  targets.reserve(files.size());
  for (const std::filesystem::path& file : files) {
    if (!file.empty()) {
      targets.push_back({file, -1});
    }
  }
  return sourceFromTargets(std::move(targets));
}

Source singleSource(const PlaybackTarget& target) {
  std::vector<PlaybackTarget> targets;
  if (!target.file.empty()) {
    targets.push_back(target);
  }
  return sourceFromTargets(std::move(targets));
}

Transition start(Source source, const PlaybackTarget& target) {
  return Start{std::move(source), target};
}

Transition continueWith(const PlaybackTarget& target) {
  return Continue{target};
}

Controller::Controller(ResolvePathTarget resolvePathTarget)
    : resolvePathTarget_(std::move(resolvePathTarget)) {}

bool Controller::matchesTarget(const Entry& entry, const PlaybackTarget& target,
                               const PathIdentity& targetIdentity) {
  if (target.file.empty() || entry.fileIdentity != targetIdentity) {
    return false;
  }

  // File sources represent a container as one queue item. Once that item
  // resolves to a concrete internal track, it still identifies the same queue
  // position. Track-browser sources carry exact track identities.
  return entry.target.trackIndex < 0 ||
         entry.target.trackIndex == target.trackIndex;
}

std::optional<std::size_t> Controller::findTarget(
    const std::vector<Entry>& entries, const PlaybackTarget& target) {
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

bool Controller::applyStart(Start transition) {
  std::vector<Entry> candidate;
  candidate.reserve(transition.source.targets_.size());
  for (PlaybackTarget& target : transition.source.targets_) {
    if (target.file.empty()) {
      continue;
    }
    Entry entry;
    entry.target = std::move(target);
    entry.fileIdentity = makePathIdentity(entry.target.file);
    candidate.push_back(std::move(entry));
  }

  const std::optional<std::size_t> currentIndex =
      findTarget(candidate, transition.target);
  if (!currentIndex) {
    return false;
  }

  entries_ = std::move(candidate);
  currentIndex_ = currentIndex;
  return true;
}

bool Controller::applyContinue(const Continue& transition) {
  const std::optional<std::size_t> index =
      findTarget(entries_, transition.target);
  if (!index) {
    return false;
  }
  currentIndex_ = index;
  return true;
}

bool Controller::apply(Transition transition) {
  if (Start* startTransition = std::get_if<Start>(&transition)) {
    return applyStart(std::move(*startTransition));
  }
  return applyContinue(std::get<Continue>(transition));
}

std::optional<PlaybackTarget> Controller::adjacent(Direction direction) const {
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

}  // namespace playback_controller
