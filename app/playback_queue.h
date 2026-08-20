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

namespace playback_queue {

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
  friend class Queue;
};

Source sourceFromTargets(std::vector<PlaybackTarget> targets);
Source sourceFromFiles(const std::vector<std::filesystem::path>& files);
Source singleSource(const PlaybackTarget& target);

class Queue {
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

 public:
  class PreparedActivation {
   public:
    PreparedActivation(PreparedActivation&&) noexcept = default;
    PreparedActivation& operator=(PreparedActivation&&) noexcept = default;

    PreparedActivation(const PreparedActivation&) = delete;
    PreparedActivation& operator=(const PreparedActivation&) = delete;

    [[nodiscard]] const playback_route::Route& route() const noexcept {
      return route_;
    }

   private:
    PreparedActivation(playback_route::Route route, PlaybackState state)
        : route_(std::move(route)), state_(std::move(state)) {}

    playback_route::Route route_;
    PlaybackState state_;

    friend class Queue;
  };

  using ResolvePathTarget = std::function<std::optional<PlaybackTarget>(
      const std::filesystem::path&)>;
  using ResolveRoute =
      std::function<playback_route::Route(const PlaybackTarget&)>;

  struct Services {
    ResolvePathTarget resolvePathTarget;
    ResolveRoute resolveRoute;
  };

  explicit Queue(Services services);

  [[nodiscard]] std::optional<PreparedActivation> prepareStart(
      playback_route::Route route, Source source) const;
  [[nodiscard]] std::optional<PreparedActivation> prepareTransport(
      Direction direction) const;
  void commit(PreparedActivation activation) noexcept;

 private:
  struct AdjacentTarget {
    PlaybackTarget target;
    std::size_t index = 0;
  };

  static bool matchesTarget(const Entry& entry, const PlaybackTarget& target,
                            const PathIdentity& targetIdentity);
  static std::optional<std::size_t> findTarget(
      const std::vector<Entry>& entries, const PlaybackTarget& target);

  std::optional<PreparedActivation> prepareTransport(const PlaybackState& state,
                                                     Direction direction) const;
  std::optional<AdjacentTarget> adjacent(const PlaybackState& state,
                                         Direction direction) const;

  ResolvePathTarget resolvePathTarget_;
  ResolveRoute resolveRoute_;
  std::optional<PlaybackState> activeState_;
};

}  // namespace playback_queue
