#include "../radio.h"

#include <algorithm>
#include <cassert>
#include <cmath>

void RadioToneNode::init(Radio1938& radio, RadioInitContext&) {
  auto& tone = radio.tone;
  tone.presence.setPeaking(radio.sampleRate, tone.presenceHz, tone.presenceQ,
                           tone.presenceGainDb);
  tone.tiltLp.setLowpass(radio.sampleRate, tone.tiltSplitHz, kRadioBiquadQ);
}

void RadioToneNode::reset(Radio1938& radio) {
  auto& tone = radio.tone;
  tone.presence.reset();
  tone.tiltLp.reset();
}

float RadioToneNode::process(Radio1938& radio,
                             float y,
                             const RadioSampleContext&) {
  auto& tone = radio.tone;
  if (tone.presenceHz <= 0.0f) return y;
  return tone.presence.process(y);
}

