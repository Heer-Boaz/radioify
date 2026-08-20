#include "app/playback_controller.h"

#include <utility>

namespace playback_controller {
namespace {

class DrivingGuard {
 public:
  explicit DrivingGuard(bool& driving) : driving_(driving) { driving_ = true; }
  ~DrivingGuard() { driving_ = false; }

  DrivingGuard(const DrivingGuard&) = delete;
  DrivingGuard& operator=(const DrivingGuard&) = delete;

 private:
  bool& driving_;
};

}  // namespace

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

bool Handoff::start(playback_route::Route route, Source source) {
  return startRequest_ && startRequest_(std::move(route), std::move(source));
}

bool Handoff::transport(Direction direction) {
  return transportRequest_ && transportRequest_(direction);
}

Controller::Controller(Services services)
    : resolvePathTarget_(std::move(services.resolvePathTarget)),
      resolveRoute_(std::move(services.resolveRoute)) {}

bool Controller::matchesTarget(const Entry& entry, const PlaybackTarget& target,
                               const PathIdentity& targetIdentity) {
  if (target.file.empty() || entry.fileIdentity != targetIdentity) {
    return false;
  }

  // File sources represent a container as one item. Once that item resolves to
  // a concrete internal track, it still identifies the same source position.
  // Track-browser sources carry exact track identities.
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

std::optional<Controller::PreparedActivation> Controller::prepareStart(
    playback_route::Route route, Source source) const {
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
      findTarget(candidate, route.target);
  if (!currentIndex) {
    return std::nullopt;
  }

  PreparedActivation activation;
  activation.route = std::move(route);
  activation.state.sequence =
      std::make_shared<const Sequence>(std::move(candidate));
  activation.state.currentIndex = *currentIndex;
  return activation;
}

std::optional<Controller::AdjacentTarget> Controller::adjacent(
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
    if (candidate.trackIndex >= 0) {
      return AdjacentTarget{candidate, index};
    }
    if (resolvePathTarget_) {
      if (std::optional<PlaybackTarget> resolved =
              resolvePathTarget_(candidate.file)) {
        return AdjacentTarget{std::move(*resolved), index};
      }
    }
  }
}

std::optional<Controller::PreparedActivation> Controller::prepareTransport(
    const PlaybackState& state, Direction direction) const {
  const std::optional<AdjacentTarget> target = adjacent(state, direction);
  if (!target || !resolveRoute_) {
    return std::nullopt;
  }

  PreparedActivation activation;
  activation.route = resolveRoute_(target->target);
  // Navigation owns target selection; the route service only contributes
  // presentation policy.
  activation.route.target = target->target;
  activation.state = state;
  activation.state.currentIndex = target->index;
  return activation;
}

PresentationOutcome Controller::drive(PreparedActivation activation,
                                      const Presenter& presenter) {
  if (driving_ || !presenter) {
    return PresentationOutcome::Rejected;
  }

  DrivingGuard drivingGuard(driving_);
  PreparedActivation current(std::move(activation));
  while (true) {
    std::optional<PreparedActivation> pending;
    Handoff handoff(
        [&](playback_route::Route route, Source source) {
          if (pending) {
            return false;
          }
          pending = prepareStart(std::move(route), std::move(source));
          return pending.has_value();
        },
        [&](Direction direction) {
          if (pending) {
            return false;
          }
          pending = prepareTransport(current.state, direction);
          return pending.has_value();
        });

    const PresentationOutcome outcome = presenter(current.route, handoff);
    if (outcome == PresentationOutcome::Rejected) {
      return PresentationOutcome::Rejected;
    }
    if (outcome == PresentationOutcome::Handled) {
      return pending ? PresentationOutcome::Rejected
                     : PresentationOutcome::Handled;
    }

    activeState_ = current.state;
    if (!pending) {
      return PresentationOutcome::Activated;
    }

    current = std::move(*pending);
  }
}

PresentationOutcome Controller::start(playback_route::Route route,
                                      Source source,
                                      const Presenter& presenter) {
  std::optional<PreparedActivation> activation =
      prepareStart(std::move(route), std::move(source));
  return activation ? drive(std::move(*activation), presenter)
                    : PresentationOutcome::Rejected;
}

PresentationOutcome Controller::transport(Direction direction,
                                          const Presenter& presenter) {
  if (!activeState_) {
    return PresentationOutcome::Rejected;
  }
  std::optional<PreparedActivation> activation =
      prepareTransport(*activeState_, direction);
  return activation ? drive(std::move(*activation), presenter)
                    : PresentationOutcome::Rejected;
}

}  // namespace playback_controller
