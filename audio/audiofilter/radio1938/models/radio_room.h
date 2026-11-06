#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_RADIO_ROOM_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_RADIO_ROOM_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../../math/biquad.h"

// A listening scene, separate from the receiver/cabinet response. Defaults are
// a modest headphone scene, not reconstructed Philco geometry or a measured HRTF.
struct RadioRoomConfig {
  bool enabled = true;
  bool headphones = true;
  float reflections = 0.35f;
  float decaySeconds = 0.32f;
  float lateLevel = 0.12f;
  float widthMeters = 4.0f;
  float lengthMeters = 5.0f;
  float heightMeters = 2.6f;
  float sourceXMeters = 1.4f;
  float sourceYMeters = 1.0f;
  float sourceZMeters = 1.0f;
  float listenerXMeters = 1.4f;
  float listenerYMeters = 3.0f;
  float listenerZMeters = 1.0f;
  float speakerYawDegrees = 0.0f;  // 0 faces +Y
  float headYawDegrees = 0.0f;     // 0 faces -Y
  float wallLowpassHz = 4000.0f;
};

bool validateRadioRoomConfig(const RadioRoomConfig& config, std::string* error);

struct RadioRoomSample {
  float left;
  float right;
};

class RadioRoomRenderer {
 public:
  void initialize(const RadioRoomConfig& config, float sampleRate,
                  float radiatingRadiusMeters);
  void reset();
  RadioRoomSample process(float mono);
  bool enabled() const { return enabled_; }

 private:
  struct Tap {
    size_t delay;
    float weight;
  };
  struct Path {
    std::vector<Tap> taps;
    Biquad wall;
    Biquad head;
    float gain = 0.0f;
  };
  struct ReverbLine {
    std::vector<float> buffer;
    size_t index = 0;
    float feedback = 0.0f;
    float dampingPole = 0.0f;
    float dampingState = 0.0f;
    float injection = 0.0f;
  };
  bool enabled_ = false;
  bool headphones_ = true;
  std::array<std::array<Path, 7>, 2> paths_;
  std::vector<float> history_;
  size_t writeIndex_ = 0;
  std::array<ReverbLine, 8> late_;
  float lateLevel_ = 0.0f;
  float outputGain_ = 1.0f;
  float protectionGain_ = 1.0f;
  float protectionRelease_ = 0.0f;
};

#endif
