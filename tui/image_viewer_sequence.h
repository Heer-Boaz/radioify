#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

#include "core/path_identity.h"

namespace image_viewer_sequence {

enum class Direction {
  Previous,
  Next,
};

class Sequence {
 public:
  static std::optional<Sequence> create(
      std::vector<std::filesystem::path> files,
      const std::filesystem::path& current);

  const std::filesystem::path& current() const;
  bool canMove(Direction direction) const;
  bool move(Direction direction);

 private:
  Sequence(std::vector<std::filesystem::path> files, std::size_t currentIndex)
      : files_(std::move(files)), currentIndex_(currentIndex) {}

  std::vector<std::filesystem::path> files_;
  std::size_t currentIndex_ = 0;
};

}  // namespace image_viewer_sequence
