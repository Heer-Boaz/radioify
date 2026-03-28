#include "audio_pipeline_execution.h"

#include "analysis/radio_calibration_runtime.h"

#include <algorithm>
#include <cmath>

namespace {

void prepareBlockPasses(Radio1938& radio,
                        uint32_t frames,
                        RadioBlockControl& block) {
  radio.graph.forEachOrderedPass(AudioPassDomain::Block, [&](const auto& pass) {
    if (pass.prepare) pass.prepare(radio, block, frames);
  });
}

void runControlPasses(Radio1938& radio, RadioSampleContext& ctx) {
  radio.graph.forEachOrderedPass(AudioPassDomain::SampleControl,
                                 [&](const auto& pass) {
                                   if (pass.runControl) {
                                     pass.runControl(radio, ctx);
                                   }
                                 });
}

float runProgramPasses(Radio1938& radio,
                       float y,
                       RadioSampleContext& ctx,
                       size_t firstProgramIndex) {
  const auto& order = radio.graph.order(AudioPassDomain::Program);
  const size_t begin = std::min(firstProgramIndex, order.size());
  for (size_t orderIndex = begin; orderIndex < order.size(); ++orderIndex) {
    const auto& pass = radio.graph.pass(order[orderIndex]);
    if (!pass.runSample) continue;
    const float in = y;
    y = pass.runSample(radio, y, ctx);
    if (!pass.stateOnly) {
      updatePassCalibration(radio, pass.id, in, y);
    }
  }
  return y;
}

}  // namespace

void beginRadioPipelineBlock(Radio1938& radio,
                             uint32_t frames,
                             RadioBlockControl& block) {
  radio.graph.compile();
  radio.diagnostics.reset();
  block = {};
  prepareBlockPasses(radio, frames, block);
}

float runRadioPipelineSample(Radio1938& radio,
                             float x,
                             RadioSampleContext& ctx,
                             PassId firstProgramPass) {
  runControlPasses(radio, ctx);
  const size_t firstProgramIndex =
      radio.graph.findOrderIndex(AudioPassDomain::Program, firstProgramPass);
  const float y = runProgramPasses(radio, x, ctx, firstProgramIndex);
  if (radio.calibration.enabled) {
    radio.calibration.maxDigitalOutput =
        std::max(radio.calibration.maxDigitalOutput, std::fabs(y));
  }
  return y;
}

void finishRadioPipelineBlock(Radio1938& radio) {
  if (radio.calibration.enabled) {
    updateCalibrationSnapshot(radio);
  }
}
