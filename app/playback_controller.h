#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "app/playback_route.h"
#include "core/path_identity.h"
#include "playback/target.h"

namespace playback_controller {

enum class Direction {
  Previous,
  Next,
};

enum class ActivationOutcome {
  Activated,
  Handled,
  Rejected,
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

// The command path available to a running presentation. Transport is accepted
// only after activation commits and is resolved against that committed source
// position. Opening a new source remains an application-level command.
class SessionCommands {
 public:
  SessionCommands(const SessionCommands&) = delete;
  SessionCommands& operator=(const SessionCommands&) = delete;
  SessionCommands(SessionCommands&&) = delete;
  SessionCommands& operator=(SessionCommands&&) = delete;

  [[nodiscard]] bool transport(Direction direction);

 private:
  using TransportRequest = std::function<bool(Direction)>;

  explicit SessionCommands(TransportRequest transportRequest)
      : transportRequest_(std::move(transportRequest)) {}

  TransportRequest transportRequest_;
  bool accepting_ = false;

  friend class Controller;
};

// A presentation returned here has already opened successfully. Controller
// commits the matching playback state before invoking run().
class ActivePresentation {
 public:
  virtual ~ActivePresentation() = default;
  virtual void run() = 0;
};

struct PresentationStarted {};

struct PresentationSession {
  std::unique_ptr<ActivePresentation> session;
};

struct PresentationHandled {};
struct PresentationRejected {};

using PresentationOpenResult =
    std::variant<PresentationStarted, PresentationSession, PresentationHandled,
                 PresentationRejected>;

class Presenter {
 public:
  virtual ~Presenter() = default;

  [[nodiscard]] virtual PresentationOpenResult open(
      const playback_route::Route& route, SessionCommands& commands) = 0;
};

class Controller {
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

 public:
  using ResolvePathTarget = std::function<std::optional<PlaybackTarget>(
      const std::filesystem::path&)>;
  using ResolveRoute =
      std::function<playback_route::Route(const PlaybackTarget&)>;

  struct Services {
    ResolvePathTarget resolvePathTarget;
    ResolveRoute resolveRoute;
  };

  explicit Controller(Services services);

  [[nodiscard]] ActivationOutcome start(playback_route::Route route,
                                        Source source, Presenter& presenter);
  [[nodiscard]] ActivationOutcome transport(Direction direction,
                                            Presenter& presenter);

 private:
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
  [[nodiscard]] ActivationOutcome drive(PreparedActivation activation,
                                        Presenter& presenter);

  ResolvePathTarget resolvePathTarget_;
  ResolveRoute resolveRoute_;
  std::optional<PlaybackState> activeState_;
  bool driving_ = false;
};

}  // namespace playback_controller
