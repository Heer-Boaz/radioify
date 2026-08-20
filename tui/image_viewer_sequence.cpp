#include "image_viewer_sequence.h"

#include <utility>

namespace image_viewer_sequence {

std::optional<Sequence> Sequence::create(
    std::vector<std::filesystem::path> files,
    const std::filesystem::path& current) {
  if (current.empty()) {
    return std::nullopt;
  }

  const PathIdentity currentIdentity = makePathIdentity(current);
  std::vector<std::filesystem::path> captured;
  captured.reserve(files.size());
  std::optional<std::size_t> currentIndex;
  for (std::filesystem::path& file : files) {
    if (file.empty()) {
      continue;
    }
    if (!currentIndex && makePathIdentity(file) == currentIdentity) {
      currentIndex = captured.size();
    }
    captured.push_back(std::move(file));
  }

  if (!currentIndex) {
    return std::nullopt;
  }
  return Sequence(std::move(captured), *currentIndex);
}

const std::filesystem::path& Sequence::current() const {
  return files_[currentIndex_];
}

bool Sequence::canMove(Direction direction) const {
  if (direction == Direction::Previous) {
    return currentIndex_ > 0;
  }
  return currentIndex_ + 1 < files_.size();
}

bool Sequence::move(Direction direction) {
  if (!canMove(direction)) {
    return false;
  }
  if (direction == Direction::Previous) {
    --currentIndex_;
  } else {
    ++currentIndex_;
  }
  return true;
}

}  // namespace image_viewer_sequence
