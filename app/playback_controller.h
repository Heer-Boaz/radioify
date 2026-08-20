#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "app/playback_route.h"
#include "core/path_identity.h"
#include "playback/target.h"

namespace playback_controller {

enum class Direction {
  Previous,
  Next,
};

class Source {
 public:
  Source(const Source&) = default;
  Source(Source&&) noexcept = default;
  Source& operator=(const Source&) = default;
  Source& operator=(Source&&) noexcept = default;

 private:
  explicit Source(std::vector<PlaybackTarget> targets)
      : targets_(std::move(targets)) {}

  std::vector<PlaybackTarget> targets_;

  friend Source sourceFromTargets(std::vector<PlaybackTarget> targets);
  friend class Controller;
};

Source sourceFromTargets(std::vector<PlaybackTarget> targets);
Source sourceFromFiles(const std::vector<std::filesystem::path>& files);
Source singleSource(const PlaybackTarget& target);

class Handoff {
 public:
  Handoff(const Handoff&) = delete;
  Handoff& operator=(const Handoff&) = delete;
  Handoff(Handoff&&) = delete;
  Handoff& operator=(Handoff&&) = delete;

  [[nodiscard]] bool start(playback_route::Route route, Source source);
  [[nodiscard]] bool transport(Direction direction);

 private:
  using StartRequest = std::function<bool(playback_route::Route, Source)>;
  using TransportRequest = std::function<bool(Direction)>;

  Handoff(StartRequest startRequest, TransportRequest transportRequest)
      : startRequest_(std::move(startRequest)),
        transportRequest_(std::move(transportRequest)) {}

  StartRequest startRequest_;
  TransportRequest transportRequest_;

  friend class Controller;
};

class Controller {
 public:
  using ResolvePathTarget = std::function<std::optional<PlaybackTarget>(
      const std::filesystem::path&)>;
  using ResolveRoute =
      std::function<playback_route::Route(const PlaybackTarget&)>;
  using Presenter = std::function<bool(const playback_route::Route&, Handoff&)>;

  struct Services {
    ResolvePathTarget resolvePathTarget;
    ResolveRoute resolveRoute;
  };

  explicit Controller(Services services);

  [[nodiscard]] bool start(playback_route::Route route, Source source,
                           const Presenter& presenter);
  [[nodiscard]] bool transport(Direction direction, const Presenter& presenter);

 private:
  struct Entry {
    PlaybackTarget target;
    PathIdentity fileIdentity;
  };

  struct Sequence {
    explicit Sequence(std::vector<Entry> entries)
        : entries(std::move(entries)) {}

    const std::vector<Entry> entries;
  };

  struct PlaybackState {
    std::shared_ptr<const Sequence> sequence;
    std::size_t currentIndex = 0;
  };

  struct PreparedActivation {
    playback_route::Route route;
    PlaybackState state;
  };

  struct AdjacentTarget {
    PlaybackTarget target;
    std::size_t index = 0;
  };

  static bool matchesTarget(const Entry& entry, const PlaybackTarget& target,
                            const PathIdentity& targetIdentity);
  static std::optional<std::size_t> findTarget(
      const std::vector<Entry>& entries, const PlaybackTarget& target);

  std::optional<PreparedActivation> prepareStart(playback_route::Route route,
                                                 Source source) const;
  std::optional<PreparedActivation> prepareTransport(const PlaybackState& state,
                                                     Direction direction) const;
  std::optional<AdjacentTarget> adjacent(const PlaybackState& state,
                                         Direction direction) const;
  [[nodiscard]] bool drive(PreparedActivation activation,
                           const Presenter& presenter);

  ResolvePathTarget resolvePathTarget_;
  ResolveRoute resolveRoute_;
  std::optional<PlaybackState> activeState_;
  bool driving_ = false;
};

}  // namespace playback_controller
