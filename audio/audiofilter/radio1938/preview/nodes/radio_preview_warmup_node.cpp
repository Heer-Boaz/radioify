#include "../radio_preview_pipeline.h"

#include "../../../../radio.h"

namespace {

constexpr uint32_t kPreviewWarmupFrames = 16384u;

}  // namespace

void RadioPreviewWarmupNode::init(RadioPreviewPipeline&,
                                  RadioPreviewInitContext&) {}

void RadioPreviewWarmupNode::reset(RadioPreviewPipeline& preview) {
  preview.warmedUp = false;
  preview.warmupScratch.clear();
}

void RadioPreviewWarmupNode::prepare(RadioPreviewPipeline& preview,
                                     RadioPreviewBlockControl&,
                                     uint32_t) {
  if (preview.warmedUp || !preview.radio || !preview.ingress) return;

  const auto gain = radioPreviewInitialCarrierGain(preview);
  preview.radio->warmInputCarrier(preview.ingress->receivedCarrierRmsVolts,
                                  kPreviewWarmupFrames, gain[0], gain[1]);
  preview.warmedUp = true;
}
