#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "subtitles.h"

namespace playback_subtitles {

struct SubtitleOption {
  std::filesystem::path path;
  std::string language;
  SubtitleTrack track;
  bool loaded = false;
  bool isDefault = false;
  bool isEmbedded = false;
};

class SubtitleManager {
 public:
  bool load(const std::filesystem::path& videoPath, std::string* error);

  bool hasSubtitles() const;
  const std::vector<SubtitleOption>& tracks() const;
  const SubtitleTrack* activeTrack() const;
  const SubtitleOption* activeOption() const;
  size_t trackCount() const;
 bool canCycle() const;
  bool cycleLanguage();
  std::string activeLanguageLabel() const;

 private:
  std::vector<SubtitleOption> options_;
  int activeIndex_ = -1;

  bool loadEmbeddedTracks(const std::filesystem::path& videoPath,
                         std::vector<SubtitleOption>& out,
                         std::string* error) const;
};

}  // namespace playback_subtitles
