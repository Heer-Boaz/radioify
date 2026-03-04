#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct SubtitleCue {
  int64_t startUs = 0;
  int64_t endUs = 0;
  std::string text;
  int layer = 0;
  int alignment = 2;  // ASS grid: 1..9, default bottom-center.
  float sizeScale = 1.0f;
  bool hasPosition = false;
  float posXNorm = 0.5f;
  float posYNorm = 0.9f;
  float marginVNorm = 0.0f;
  float marginLNorm = 0.0f;
  float marginRNorm = 0.0f;
};

struct SubtitleTrack {
  std::string label;
  std::vector<SubtitleCue> cues;
  mutable size_t lastCueIndex = 0;

  void cuesAt(int64_t clockUs, std::vector<const SubtitleCue*>* out) const;
  const SubtitleCue* cueAt(int64_t clockUs) const;
  void resetLookup() const;
};

class SubtitleManager {
 public:
  void loadForVideo(const std::filesystem::path& videoPath);

  size_t trackCount() const;
  size_t selectableTrackCount() const;
  bool selectFirstTrackWithCues();
  size_t activeTrackIndex() const;
  const SubtitleTrack* activeTrack() const;
  std::string activeTrackLabel() const;
  bool isActiveLastCueTrack() const;
  bool cycleLanguage();

 private:
  std::string makeUniqueLabel(const std::string& rawLabel) const;

  std::vector<SubtitleTrack> tracks_;
  size_t activeTrack_ = 0;
};

