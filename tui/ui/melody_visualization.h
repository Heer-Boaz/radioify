#pragma once

#include <cstddef>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>

#include "audio/analysis/melody_types.h"

namespace tui_melody_visualization {

struct SourceIdentity {
  std::filesystem::path file;
  std::optional<int> trackIndex;
};

struct Observation {
  SourceIdentity source;
  AudioMelodyInfo pitch;
  AudioMelodyAnalysisState analysis;
  bool playbackAdvancing = false;
};

struct Sample {
  int midiNote = -1;
  float confidence = 0.0f;
};

class Model {
public:
  explicit Model(std::size_t historyCapacity = 4096);

  bool active() const { return active_; }
  bool setActive(bool active);
  bool toggle();

  void update(Observation observation);

  const AudioMelodyInfo &pitch() const { return pitch_; }
  const AudioMelodyAnalysisState &analysis() const { return analysis_; }
  const std::deque<Sample> &history() const { return history_; }
  std::size_t historyCapacity() const { return historyCapacity_; }

private:
  void clearHistory();
  void appendSample(const AudioMelodyInfo &pitch);

  std::size_t historyCapacity_ = 0;
  bool active_ = false;
  bool hasSource_ = false;
  bool analysisRunning_ = false;
  SourceIdentity source_;
  AudioMelodyInfo pitch_;
  AudioMelodyAnalysisState analysis_;
  std::deque<Sample> history_;
};

std::string midiNoteName(int midiNote);

} // namespace tui_melody_visualization
