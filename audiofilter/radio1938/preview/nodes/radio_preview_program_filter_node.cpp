#include "../radio_preview_pipeline.h"

#include <algorithm>

void RadioPreviewProgramFilterNode::init(RadioPreviewPipeline& preview,
                                         RadioPreviewInitContext&) {
  const float safeSampleRate = std::max(preview.sampleRate, 1.0f);
  const float safeBandwidth =
      std::max(preview.config ? preview.config->programBandwidthHz : 2400.0f,
               1.0f);
  preview.programHp.setHighpass(safeSampleRate, 45.0f, kRadioBiquadQ);
  preview.programLp1.setLowpass(safeSampleRate, safeBandwidth, kRadioBiquadQ);
  preview.programLp2.setLowpass(safeSampleRate, safeBandwidth, kRadioBiquadQ);
}

void RadioPreviewProgramFilterNode::reset(RadioPreviewPipeline& preview) {
  preview.programHp.reset();
  preview.programLp1.reset();
  preview.programLp2.reset();
}

float RadioPreviewProgramFilterNode::run(RadioPreviewPipeline& preview,
                                         float y,
                                         RadioPreviewSampleContext&) {
  y = preview.programHp.process(y);
  y = preview.programLp1.process(y);
  y = preview.programLp2.process(y);
  return y;
}
