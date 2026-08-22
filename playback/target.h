#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>
#include <variant>

struct PlaybackFileTarget {
  std::filesystem::path file;
};

struct PlaybackTrackTarget {
  std::filesystem::path file;
  std::uint32_t trackIndex = 0;
};

using PlaybackTarget = std::variant<PlaybackFileTarget, PlaybackTrackTarget>;

inline PlaybackTarget playbackFileTarget(std::filesystem::path file) {
  return PlaybackFileTarget{std::move(file)};
}

inline std::optional<PlaybackTarget> playbackTrackTarget(
    std::filesystem::path file, int trackIndex) {
  if (file.empty() || trackIndex < 0) {
    return std::nullopt;
  }
  return PlaybackTrackTarget{std::move(file),
                             static_cast<std::uint32_t>(trackIndex)};
}

inline const std::filesystem::path& playbackTargetFile(
    const PlaybackTarget& target) {
  return std::visit(
      [](const auto& value) -> const std::filesystem::path& {
        return value.file;
      },
      target);
}

inline std::optional<int> playbackTargetTrackIndex(
    const PlaybackTarget& target) {
  if (const auto* track = std::get_if<PlaybackTrackTarget>(&target)) {
    return static_cast<int>(track->trackIndex);
  }
  return std::nullopt;
}

inline bool playbackTargetIsTrack(const PlaybackTarget& target) {
  return std::holds_alternative<PlaybackTrackTarget>(target);
}
