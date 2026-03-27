#include "../radio.h"

#include <algorithm>
#include <cassert>
#include <cmath>

void RadioNoiseNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& noiseConfig = radio.noiseConfig;
  auto& noiseRuntime = radio.noiseRuntime;
  noiseRuntime.hum.setFs(radio.sampleRate, initCtx.tunedBw);
  noiseRuntime.hum.humHz = noiseConfig.humHzDefault;
}

void RadioNoiseNode::reset(Radio1938& radio) { radio.noiseRuntime.hum.reset(); }

float RadioNoiseNode::process(Radio1938& radio,
                              float y,
                              const RadioSampleContext& ctx) {
  NoiseInput noiseIn{};
  noiseIn.programSample = y;
  noiseIn.noiseAmp = ctx.derived.noiseAmp;
  noiseIn.crackleAmp = ctx.derived.crackleAmp;
  noiseIn.crackleRate = ctx.derived.crackleRate;
  noiseIn.humAmp = ctx.derived.humAmp;
  noiseIn.humToneEnabled = ctx.derived.humToneEnabled;
  return y + radio.noiseRuntime.hum.process(noiseIn);
}

