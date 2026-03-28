#include "../../../radio.h"

#include <algorithm>
#include <cassert>
#include <cmath>

void RadioToneNode::init(Radio1938& radio, RadioInitContext&) {
  auto& tone = radio.tone;
  if (tone.presenceHz > 0.0f && std::fabs(tone.presenceGainDb) > 1e-3f) {
    tone.presence.setPeaking(radio.sampleRate, tone.presenceHz, tone.presenceQ,
                             tone.presenceGainDb);
  } else {
    tone.presence = Biquad{};
  }
  if (tone.tiltSplitHz > 0.0f && std::fabs(tone.tiltDepthDb) > 1e-3f) {
    tone.tiltLp.setLowpass(radio.sampleRate, tone.tiltSplitHz, kRadioBiquadQ);
  } else {
    tone.tiltLp = Biquad{};
  }
}

void RadioToneNode::reset(Radio1938& radio) {
  auto& tone = radio.tone;
  tone.presence.reset();
  tone.tiltLp.reset();
}

float RadioToneNode::run(Radio1938& radio, float y, RadioSampleContext&) {
  auto& tone = radio.tone;
  float out = y;
  if (tone.tiltSplitHz > 0.0f && std::fabs(tone.tiltDepthDb) > 1e-3f) {
    float low = tone.tiltLp.process(out);
    float high = out - low;
    float lowGain = std::pow(10.0f, -tone.tiltDepthDb / 40.0f);
    float highGain = std::pow(10.0f, tone.tiltDepthDb / 40.0f);
    out = lowGain * low + highGain * high;
  }
  if (tone.presenceHz > 0.0f && std::fabs(tone.presenceGainDb) > 1e-3f) {
    out = tone.presence.process(out);
  }
  return out;
}
