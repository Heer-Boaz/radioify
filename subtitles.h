#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct SubtitleCue {
  int64_t startUs = 0;
  int64_t endUs = 0;
  std::string text;
};

class SubtitleTrack {
 public:
  bool loadFromFile(const std::filesystem::path& path, std::string* error);
  void clear();
  bool empty() const { return cues_.empty(); }
  size_t size() const { return cues_.size(); }
  const SubtitleCue* cueAt(int64_t timeUs) const;

 private:
  std::vector<SubtitleCue> cues_;
};
