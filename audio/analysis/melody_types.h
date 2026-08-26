#pragma once

#include <cstddef>
#include <string>

struct AudioMelodyInfo {
  float frequencyHz = 0.0f;
  float confidence = 0.0f;
  int midiNote = -1;
};

struct AudioMelodyAnalysisState {
  bool ready = false;
  bool running = false;
  float progress = 0.0f;
  std::size_t frameCount = 0;
  std::string error;
};
