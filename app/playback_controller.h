#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

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

struct Start {
  Source source;
  PlaybackTarget target;
};

struct Continue {
  PlaybackTarget target;
};

using Transition = std::variant<Start, Continue>;

Transition start(Source source, const PlaybackTarget& target);
Transition continueWith(const PlaybackTarget& target);

class Controller {
 public:
  using ResolvePathTarget = std::function<std::optional<PlaybackTarget>(
      const std::filesystem::path&)>;

  explicit Controller(ResolvePathTarget resolvePathTarget);

  [[nodiscard]] bool apply(Transition transition);
  std::optional<PlaybackTarget> adjacent(Direction direction) const;

 private:
  struct Entry {
    PlaybackTarget target;
    PathIdentity fileIdentity;
  };

  static bool matchesTarget(const Entry& entry, const PlaybackTarget& target,
                            const PathIdentity& targetIdentity);
  static std::optional<std::size_t> findTarget(
      const std::vector<Entry>& entries, const PlaybackTarget& target);

  [[nodiscard]] bool applyStart(Start transition);
  [[nodiscard]] bool applyContinue(const Continue& transition);

  ResolvePathTarget resolvePathTarget_;
  std::vector<Entry> entries_;
  std::optional<std::size_t> currentIndex_;
};

}  // namespace playback_controller
