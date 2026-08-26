#include "tui/ui/melody_visualization.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <utility>

namespace {

bool expect(bool condition, const char *message) {
  if (condition) {
    return true;
  }
  std::cerr << "melody_visualization_tests: " << message << '\n';
  return false;
}

tui_melody_visualization::Observation
observation(const std::filesystem::path &file, int midiNote, float confidence,
            bool playbackAdvancing = true, std::optional<int> trackIndex = 0,
            bool analysisRunning = false) {
  tui_melody_visualization::Observation result;
  result.source.file = file;
  result.source.trackIndex = trackIndex;
  result.pitch.frequencyHz = 440.0f;
  result.pitch.confidence = confidence;
  result.pitch.midiNote = midiNote;
  result.analysis.running = analysisRunning;
  result.playbackAdvancing = playbackAdvancing;
  return result;
}

} // namespace

int main() {
  using tui_melody_visualization::Model;

  bool ok = true;
  Model model(3);
  ok &= expect(!model.active(), "the pitch monitor must start closed");
  ok &= expect(model.toggle(), "toggle must report the new active state");

  model.update(observation("track-a.mp3", 60, 1.5f));
  model.update(observation("track-a.mp3", 61, 0.5f));
  model.update(observation("track-a.mp3", 62, -0.5f));
  model.update(observation("track-a.mp3", 63, 0.8f));
  ok &= expect(model.history().size() == 3,
               "history must stay within its configured capacity");
  ok &= expect(model.history().front().midiNote == 61 &&
                   model.history().back().midiNote == 63,
               "history eviction must preserve sample ordering");
  ok &= expect(model.history()[1].confidence == 0.0f,
               "sample confidence must be normalized before storage");

  model.update(observation("track-a.mp3", 64, 0.7f, false));
  ok &= expect(model.history().size() == 3,
               "paused playback must not synthesize history samples");

  model.update(observation("track-b.mp3", 65, 0.7f, false));
  ok &= expect(model.history().empty(),
               "changing the playback source must reset history");
  model.update(observation("track-b.mp3", 66, 0.7f, true, 0));
  model.update(observation("track-b.mp3", 67, 0.7f, false, 1));
  ok &= expect(model.history().empty(),
               "changing an embedded track must reset history");

  model.update(observation("track-b.mp3", 68, 0.7f, true, 1, false));
  model.update(observation("track-b.mp3", 69, 0.7f, false, 1, true));
  ok &= expect(model.history().empty(),
               "starting offline analysis must reset live history once");
  model.update(observation("track-b.mp3", 70, 0.7f, true, 1, true));
  ok &= expect(model.history().size() == 1,
               "a running analysis must not repeatedly clear history");

  auto invalid =
      observation("track-b.mp3", 200, std::numeric_limits<float>::quiet_NaN(),
                  false, 1, true);
  invalid.pitch.frequencyHz = std::numeric_limits<float>::infinity();
  invalid.analysis.progress = std::numeric_limits<float>::quiet_NaN();
  model.update(std::move(invalid));
  ok &=
      expect(model.pitch().midiNote == -1 && model.pitch().confidence == 0.0f &&
                 model.pitch().frequencyHz == 0.0f &&
                 model.analysis().progress == 0.0f,
             "non-finite and out-of-range analyzer output must be safe");

  ok &= expect(tui_melody_visualization::midiNoteName(60) == "C4" &&
                   tui_melody_visualization::midiNoteName(69) == "A4" &&
                   tui_melody_visualization::midiNoteName(-1) == "??",
               "MIDI note labels must use standard scientific pitch names");

  ok &= expect(model.setActive(false) && !model.active(),
               "the component must own its visible state");
  ok &= expect(!model.setActive(false),
               "setting the existing visible state must be idempotent");

  Model disabledHistory(0);
  disabledHistory.setActive(true);
  disabledHistory.update(observation("track.mp3", 60, 1.0f));
  ok &= expect(disabledHistory.history().empty(),
               "zero capacity must disable history without special cases");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
