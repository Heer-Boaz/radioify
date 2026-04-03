#include "radio_preview_pipeline.h"

#include "../../../radio.h"
#include "../radio_buffer_io.h"

#include <algorithm>

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
      preview.graph.findOrderIndex(AudioPassDomain::Program, firstProgramPass);
  if (block) {
    block->cachedProgramStartPass = firstProgramPass;
    block->cachedProgramStartIndex = orderIndex;
    block->programStartCached = true;
  }
  return orderIndex;
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
  config = &previewConfig;
  sampleRate = newSampleRate;
  RadioPreviewInitContext initCtx{};
  lifecycle.initialize(*this, initCtx);
  graph.compile();
  reset();
}

void RadioPreviewPipeline::reset() {
  lifecycle.reset(*this);
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
  for (uint32_t frame = 0; frame < frames; ++frame) {
    RadioPreviewSampleContext ctx{};
    ctx.block = &block;
    const float mono = sampleInterleavedToMono(samples, frame,
                                               static_cast<int>(channels));
    filteredScratch[frame] = runRadioPreviewPipelineSample(
        *this, mono, ctx, RadioPreviewPassId::ProgramFilter);
  }

  radioRef.processAmAudio(filteredScratch.data(), filteredScratch.data(), frames,
                          ingress->receivedCarrierRmsVolts,
                          ingress->modulationIndex);

  for (uint32_t frame = 0; frame < frames; ++frame) {
    writeMonoToInterleaved(samples, frame, static_cast<int>(channels),
                           filteredScratch[frame]);
  }
}
