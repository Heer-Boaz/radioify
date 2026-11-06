#include "radio_preview_pipeline.h"

#include "../../../radio.h"
#include "../radio_buffer_io.h"

#include <algorithm>
#include <cassert>

namespace {

void prepareBlockPasses(RadioPreviewPipeline& preview,
                        uint32_t frames,
                        RadioPreviewBlockControl& block) {
  preview.graph.forEachOrderedPass(AudioPassDomain::Block,
                                   [&](const auto& pass) {
                                     if (pass.prepare) {
                                       pass.prepare(preview, block, frames);
                                     }
                                   });
}

float runProgramPasses(RadioPreviewPipeline& preview,
                       float y,
                       RadioPreviewSampleContext& ctx,
                       size_t firstProgramIndex) {
  const auto& order = preview.graph.order(AudioPassDomain::Program);
  const size_t begin = std::min(firstProgramIndex, order.size());
  for (size_t orderIndex = begin; orderIndex < order.size(); ++orderIndex) {
    const auto& pass = preview.graph.pass(order[orderIndex]);
    if (!pass.runSample) continue;
    y = pass.runSample(preview, y, ctx);
  }
  return y;
}

size_t resolveFirstProgramIndex(RadioPreviewPipeline& preview,
                                const RadioPreviewSampleContext& ctx,
                                RadioPreviewPassId firstProgramPass) {
  const RadioPreviewBlockControl* block = ctx.block;
  if (block && block->programStartCached &&
      block->cachedProgramStartPass == firstProgramPass) {
    return block->cachedProgramStartIndex;
  }
  const size_t orderIndex =
      preview.graph.findEntryOrderIndex(AudioPassDomain::Program, firstProgramPass);
  if (block) {
    block->cachedProgramStartPass = firstProgramPass;
    block->cachedProgramStartIndex = orderIndex;
    block->programStartCached = true;
  }
  return orderIndex;
}

void applyPreviewRuntimeBudget(Radio1938& radioRef,
                               const RadioPreviewConfig& previewConfig) {
  radioRef.demod.am.waveformMaxSubsteps =
      std::max(1, previewConfig.detectorMaxSubsteps);
}

}  // namespace

void beginRadioPreviewPipelineBlock(RadioPreviewPipeline& preview,
                                    uint32_t frames,
                                    RadioPreviewBlockControl& block) {
  preview.graph.compile();
  block = {};
  prepareBlockPasses(preview, frames, block);
}

float runRadioPreviewPipelineSample(RadioPreviewPipeline& preview,
                                    float x,
                                    RadioPreviewSampleContext& ctx,
                                    RadioPreviewPassId firstProgramPass) {
  const size_t firstProgramIndex =
      resolveFirstProgramIndex(preview, ctx, firstProgramPass);
  return runProgramPasses(preview, x, ctx, firstProgramIndex);
}

void RadioPreviewPipeline::initialize(Radio1938& radioRef,
                                      const RadioAmIngressConfig& ingressConfig,
                                      const RadioPreviewConfig& previewConfig,
                                      float newSampleRate) {
  radio = &radioRef;
  ingress = &ingressConfig;
  sampleRate = newSampleRate;
  assert(radioRef.tuning.tunedBw > 0.0f &&
         "Preview requires an initialized Radio1938 bandwidth");
  audioBandwidthHz = radioRef.tuning.sourceBandwidthHz;
  // Effective piston radii are size-family priors, not measured cone areas.
  roomRenderer.initialize(radioRef.room, newSampleRate,
      radioRef.receiverProfile == RadioReceiverProfile::Typical1930s ? .05f : .14f);
  applyPreviewRuntimeBudget(radioRef, previewConfig);
  RadioPreviewInitContext initCtx{};
  lifecycle.initialize(*this, initCtx);
  graph.compile();
  reset();
}

void RadioPreviewPipeline::reset() {
  lifecycle.reset(*this);
  roomRenderer.reset();
}

void RadioPreviewPipeline::reserveBlockFrames(uint32_t frames) {
  filteredScratch.reserve(frames);
  receptionScratch.reserve(frames);
}

void RadioPreviewPipeline::runBlock(Radio1938& radioRef,
                                    float* samples,
                                    uint32_t frames,
                                    uint32_t channels) {
  if (!samples || frames == 0 || channels == 0 || !ingress) return;

  radio = &radioRef;
  RadioPreviewBlockControl block{};
  beginRadioPreviewPipelineBlock(*this, frames, block);

  filteredScratch.resize(frames);
  receptionScratch.resize(frames);
  RadioPreviewSampleContext ctx{};
  ctx.block = &block;
  const size_t programStartIndex =
      resolveFirstProgramIndex(*this, ctx, RadioPreviewPassId::BroadcastSource);
  for (uint32_t frame = 0; frame < frames; ++frame) {
    ctx.reception = &receptionScratch[frame];
    const float mono = sampleInterleavedToMono(samples, frame,
                                               static_cast<int>(channels));
    filteredScratch[frame] = runProgramPasses(*this, mono, ctx,
                                              programStartIndex);
  }

  radioRef.processAmAudio(filteredScratch.data(), filteredScratch.data(), frames,
                          ingress->receivedCarrierRmsVolts,
                          ingress->modulationIndex,
                          ingress->reception.enabled
                              ? receptionScratch.data()
                              : nullptr);

  for (uint32_t frame = 0; frame < frames; ++frame) {
    const auto spatial = roomRenderer.process(filteredScratch[frame]);
    const float mono = .5f*(spatial.left+spatial.right);
    const size_t base = static_cast<size_t>(frame)*channels;
    if (channels == 1) {
      samples[base] = mono;
    } else {
      samples[base] = spatial.left;
      samples[base+1] = spatial.right;
      for (uint32_t channel=2; channel<channels; ++channel) samples[base+channel] = mono;
    }
  }
}
