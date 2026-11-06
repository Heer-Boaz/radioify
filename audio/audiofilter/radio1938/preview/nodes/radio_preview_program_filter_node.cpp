#include "../radio_preview_pipeline.h"

#include <algorithm>

void RadioPreviewProgramFilterNode::init(RadioPreviewPipeline& preview,
                                         RadioPreviewInitContext&) {
  const float safeSampleRate = std::max(preview.sampleRate, 1.0f);
  preview.programHp.setHighpass(safeSampleRate, 45.0f, kRadioBiquadQ);
  preview.programLp.setLowpass(safeSampleRate, preview.audioBandwidthHz,
                               kRadioBiquadQ);
  // The real, sample-rate-scaled AM representation needs bandlimited source
  // audio as well as downmix image rejection. Otherwise e.g. a 20 kHz source
  // can fold to 4 kHz around the 12 kHz virtual carrier at 48 kHz sampling.
  const float pass = preview.audioBandwidthHz / safeSampleRate;
  preview.programImageReject.configure(pass, .5f - pass);
}

void RadioPreviewProgramFilterNode::reset(RadioPreviewPipeline& preview) {
  preview.programHp.reset();
  preview.programLp.reset();
  preview.programImageReject.reset();
}

float RadioPreviewProgramFilterNode::run(RadioPreviewPipeline& preview,
                                         float y,
                                         RadioPreviewSampleContext&) {
  y = preview.programHp.process(y);
  y = preview.programLp.process(y);
  return preview.programImageReject.process(y, 0)[0];
}
