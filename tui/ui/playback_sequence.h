#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "path_identity.h"
#include "playback_target.h"

class PlaybackSequence {
 public:
  void replace(std::vector<PlaybackTarget> targets,
               const PlaybackTarget& current);
  void replaceWithSingle(const PlaybackTarget& current);
  void clear();

  bool select(const PlaybackTarget& target);
  std::optional<PlaybackTarget> adjacent(int direction,
                                         std::size_t distance = 1) const;

  std::size_t size() const { return entries_.size(); }
  bool empty() const { return entries_.empty(); }

 private:
  struct Entry {
    PlaybackTarget target;
    PathIdentity fileIdentity;
  };

  static bool matchesTarget(const Entry& entry, const PlaybackTarget& target,
                            const PathIdentity& targetIdentity);

  std::vector<Entry> entries_;
  std::optional<std::size_t> currentIndex_;
};
