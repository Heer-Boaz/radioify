#include "melody_visualization.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace tui_melody_visualization {
namespace {

bool sameSource(const SourceIdentity &left, const SourceIdentity &right) {
  return left.file == right.file && left.trackIndex == right.trackIndex;
}

float finiteUnitValue(float value) {
  return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

AudioMelodyInfo normalizePitch(AudioMelodyInfo pitch) {
  if (!std::isfinite(pitch.frequencyHz) || pitch.frequencyHz < 0.0f) {
    pitch.frequencyHz = 0.0f;
  }
  pitch.confidence = finiteUnitValue(pitch.confidence);
  if (pitch.midiNote < 0 || pitch.midiNote > 127) {
    pitch.midiNote = -1;
  }
  return pitch;
}

AudioMelodyAnalysisState normalizeAnalysis(AudioMelodyAnalysisState analysis) {
  analysis.progress = finiteUnitValue(analysis.progress);
  return analysis;
}

} // namespace

Model::Model(std::size_t historyCapacity) : historyCapacity_(historyCapacity) {}

bool Model::setActive(bool active) {
  if (active_ == active) {
    return false;
  }
  active_ = active;
  if (active_) {
    clearHistory();
  }
  return true;
}

bool Model::toggle() {
  setActive(!active_);
  return active_;
}

void Model::update(Observation observation) {
  const bool sourceChanged =
      !hasSource_ || !sameSource(source_, observation.source);
  const bool analysisStarted =
      observation.analysis.running && !analysisRunning_;
  if (sourceChanged || analysisStarted) {
    clearHistory();
  }

  source_ = std::move(observation.source);
  hasSource_ = true;
  analysisRunning_ = observation.analysis.running;
  pitch_ = normalizePitch(observation.pitch);
  analysis_ = normalizeAnalysis(std::move(observation.analysis));

  if (active_ && observation.playbackAdvancing) {
    appendSample(pitch_);
  }
}

void Model::clearHistory() { history_.clear(); }

void Model::appendSample(const AudioMelodyInfo &pitch) {
  if (historyCapacity_ == 0) {
    return;
  }
  history_.push_back({pitch.midiNote, pitch.confidence});
  while (history_.size() > historyCapacity_) {
    history_.pop_front();
  }
}

std::string midiNoteName(int midiNote) {
  if (midiNote < 0 || midiNote > 127) {
    return "??";
  }
  static constexpr const char *kNoteNames[] = {
      "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  const int note = midiNote % 12;
  const int octave = midiNote / 12 - 1;
  return std::string(kNoteNames[static_cast<std::size_t>(note)]) +
         std::to_string(octave);
}

} // namespace tui_melody_visualization
