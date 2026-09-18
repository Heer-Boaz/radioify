#include "app/playback_queue.h"

#include <algorithm>
#include <system_error>
#include <utility>

#include "audio/media_formats.h"

namespace playback_queue {

Source sourceFromTargets(std::vector<PlaybackTarget> targets) {
  return Source(std::move(targets));
}

Source sourceFromFiles(const std::vector<std::filesystem::path>& files) {
  std::vector<PlaybackTarget> targets;
  targets.reserve(files.size());
  for (const std::filesystem::path& file : files) {
    if (!file.empty()) {
      targets.push_back(playbackFileTarget(file));
    }
  }
  return sourceFromTargets(std::move(targets));
}

Source sourceFromFileNeighbourhood(const std::filesystem::path& file) {
  if (file.empty()) {
    return sourceFromTargets({});
  }

  const std::filesystem::path directory = file.parent_path();
  std::vector<std::filesystem::path> siblings;
  if (!directory.empty()) {
    std::error_code scanError;
    std::filesystem::directory_iterator entry(
        directory, std::filesystem::directory_options::skip_permission_denied,
        scanError);
    const std::filesystem::directory_iterator end;
    while (!scanError && entry != end) {
      const std::filesystem::path& candidate = entry->path();
      std::error_code kindError;
      if (entry->is_regular_file(kindError) && !kindError &&
          (isSupportedAudioExt(candidate) || isSupportedVideoExt(candidate))) {
        siblings.push_back(candidate);
      }
      entry.increment(scanError);
    }
  }

  // A folder that cannot be read, or that does not list the opened file back,
  // must still yield a playable queue: the file itself is the fallback.
  const bool containsOpenedFile =
      std::any_of(siblings.begin(), siblings.end(),
                  [&file](const std::filesystem::path& sibling) {
                    return samePath(sibling, file);
                  });
  if (!containsOpenedFile) {
    return sourceFromFiles({file});
  }

  std::sort(siblings.begin(), siblings.end(),
            [](const std::filesystem::path& left,
               const std::filesystem::path& right) {
              return pathIdentityKey(makePathIdentity(left)) <
                     pathIdentityKey(makePathIdentity(right));
            });
  return sourceFromFiles(siblings);
}

Source singleSource(const PlaybackTarget& target) {
  std::vector<PlaybackTarget> targets;
  if (!playbackTargetFile(target).empty()) {
    targets.push_back(target);
  }
  return sourceFromTargets(std::move(targets));
}

Queue::Queue(Services services)
    : resolvePathTarget_(std::move(services.resolvePathTarget)),
      resolveRoute_(std::move(services.resolveRoute)) {}

bool Queue::matchesTarget(const Entry& entry, const PlaybackTarget& target,
                          const PathIdentity& targetIdentity) {
  if (playbackTargetFile(target).empty() ||
      entry.fileIdentity != targetIdentity) {
    return false;
  }

  // File sources represent a container as one item. Once that item resolves to
  // a concrete internal track, it still identifies the same source position.
  // Track-browser sources carry exact track identities.
  const std::optional<int> sourceTrack =
      playbackTargetTrackIndex(entry.target);
  return !sourceTrack || sourceTrack == playbackTargetTrackIndex(target);
}

std::optional<std::size_t> Queue::findTarget(const std::vector<Entry>& entries,
                                             const PlaybackTarget& target) {
  if (playbackTargetFile(target).empty()) {
    return std::nullopt;
  }

  const PathIdentity targetIdentity =
      makePathIdentity(playbackTargetFile(target));
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (matchesTarget(entries[index], target, targetIdentity)) {
      return index;
    }
  }
  return std::nullopt;
}

std::optional<Queue::PreparedActivation> Queue::prepareStart(
    playback_route::Route route, Source source) const {
  std::vector<Entry> candidate;
  candidate.reserve(source.targets_.size());
  for (PlaybackTarget& target : source.targets_) {
    if (playbackTargetFile(target).empty()) {
      continue;
    }
    Entry entry;
    entry.target = std::move(target);
    entry.fileIdentity = makePathIdentity(playbackTargetFile(entry.target));
    candidate.push_back(std::move(entry));
  }

  const std::optional<std::size_t> currentIndex =
      findTarget(candidate, route.target);
  if (!currentIndex) {
    return std::nullopt;
  }

  PlaybackState state;
  state.sequence = std::make_shared<const Sequence>(std::move(candidate));
  state.currentIndex = *currentIndex;
  return PreparedActivation(std::move(route), std::move(state));
}

std::optional<Queue::AdjacentTarget> Queue::adjacent(
    const PlaybackState& state, Direction direction) const {
  if (!state.sequence || state.currentIndex >= state.sequence->entries.size()) {
    return std::nullopt;
  }

  const std::vector<Entry>& entries = state.sequence->entries;
  std::size_t index = state.currentIndex;
  while (true) {
    if (direction == Direction::Previous) {
      if (index == 0) {
        return std::nullopt;
      }
      --index;
    } else {
      if (index + 1 >= entries.size()) {
        return std::nullopt;
      }
      ++index;
    }

    const PlaybackTarget& candidate = entries[index].target;
    if (playbackTargetIsTrack(candidate)) {
      return AdjacentTarget{candidate, index};
    }
    if (std::optional<PlaybackTarget> resolved =
            resolvePathTarget_(playbackTargetFile(candidate))) {
      return AdjacentTarget{std::move(*resolved), index};
    }
  }
}

std::optional<Queue::PreparedActivation> Queue::prepareTransport(
    const PlaybackState& state, Direction direction) const {
  const std::optional<AdjacentTarget> target = adjacent(state, direction);
  if (!target) {
    return std::nullopt;
  }

  playback_route::Route route = resolveRoute_(target->target);
  // Navigation owns target selection; the route service only contributes
  // presentation policy.
  route.target = target->target;
  PlaybackState candidateState = state;
  candidateState.currentIndex = target->index;
  return PreparedActivation(std::move(route), std::move(candidateState));
}

std::optional<Queue::PreparedActivation> Queue::prepareTransport(
    Direction direction) const {
  if (!activeState_) {
    return std::nullopt;
  }
  return prepareTransport(*activeState_, direction);
}

void Queue::commit(PreparedActivation activation) noexcept {
  activeState_ = std::move(activation.state_);
}

}  // namespace playback_queue
