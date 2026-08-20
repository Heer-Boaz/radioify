#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "path_identity.h"
#include "playback_target.h"

namespace playback_queue {

enum class Direction {
  Previous,
  Next,
};

class Source {
 private:
  explicit Source(std::vector<PlaybackTarget> targets)
      : targets_(std::move(targets)) {}

  std::vector<PlaybackTarget> targets_;

  friend Source fromTargets(std::vector<PlaybackTarget> targets);
  friend class Queue;
};

Source fromTargets(std::vector<PlaybackTarget> targets);
Source fromFiles(const std::vector<std::filesystem::path>& files);
Source single(const PlaybackTarget& target);

class Queue {
 public:
  using ResolvePathTarget = std::function<std::optional<PlaybackTarget>(
      const std::filesystem::path&)>;

  explicit Queue(ResolvePathTarget resolvePathTarget);

  // Replaces the active source only when it contains current. Failed
  // activations leave the previous queue untouched.
  [[nodiscard]] bool activate(Source source, const PlaybackTarget& current);
  [[nodiscard]] bool select(const PlaybackTarget& target);

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

  ResolvePathTarget resolvePathTarget_;
  std::vector<Entry> entries_;
  std::optional<std::size_t> currentIndex_;
};

}  // namespace playback_queue
