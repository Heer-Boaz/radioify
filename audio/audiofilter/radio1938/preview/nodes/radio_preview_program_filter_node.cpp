#include "../radio_preview_pipeline.h"

#include <algorithm>

void RadioPreviewProgramFilterNode::init(RadioPreviewPipeline& preview,
                                         RadioPreviewInitContext&) {
  const float safeSampleRate = std::max(preview.sampleRate, 1.0f);
  preview.programHp.setHighpass(safeSampleRate, 45.0f, kRadioBiquadQ);
  preview.programLp.setLowpass(safeSampleRate, preview.audioBandwidthHz,
                               kRadioBiquadQ);
}

void RadioPreviewProgramFilterNode::reset(RadioPreviewPipeline& preview) {
  preview.programHp.reset();
  preview.programLp.reset();
}

float RadioPreviewProgramFilterNode::run(RadioPreviewPipeline& preview,
                                         float y,
                                         RadioPreviewSampleContext&) {
  y = preview.programHp.process(y);
  y = preview.programLp.process(y);
  return y;
}
