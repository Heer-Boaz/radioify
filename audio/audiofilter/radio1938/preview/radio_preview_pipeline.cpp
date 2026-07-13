#include "radio_preview_pipeline.h"

#include "../../graph/audio_pipeline.h"

namespace {

using RadioPreviewPipelineDefinition =
    AudioPipeline<RadioPreviewPipeline,
                  RadioPreviewBlockControl,
                  RadioPreviewSampleContext,
                  RadioPreviewInitContext,
                  RadioPreviewPassId>;

const RadioPreviewPipelineDefinition& radioPreviewPipelineDefinition() {
  static const auto pipeline = [] {
    AudioPipelineBuilder<RadioPreviewPipeline,
                         RadioPreviewBlockControl,
                         RadioPreviewSampleContext,
                         RadioPreviewInitContext,
                         RadioPreviewPassId>
        builder;
    builder.block(RadioPreviewPassId::Warmup, "CarrierWarmup")
        .init(&RadioPreviewWarmupNode::init)
        .reset(&RadioPreviewWarmupNode::reset)
        .prepare(&RadioPreviewWarmupNode::prepare);

    builder.program(RadioPreviewPassId::BroadcastSource, "BroadcastSource")
        .init(&RadioPreviewBroadcastSourceNode::init)
        .reset(&RadioPreviewBroadcastSourceNode::reset)
        .run(&RadioPreviewBroadcastSourceNode::run);

    builder.program(RadioPreviewPassId::ProgramFilter, "ProgramFilter")
        .init(&RadioPreviewProgramFilterNode::init)
        .reset(&RadioPreviewProgramFilterNode::reset)
        .dependsOn(RadioPreviewPassId::BroadcastSource)
        .run(&RadioPreviewProgramFilterNode::run);

    return builder.build(static_cast<size_t>(RadioPreviewPassId::ProgramFilter) +
                         1u);
  }();
  return pipeline;
}

}  // namespace

RadioPreviewGraph makeRadioPreviewGraph() {
  return radioPreviewPipelineDefinition().buildGraph();
}

RadioPreviewLifecycle makeRadioPreviewLifecycle() {
  return radioPreviewPipelineDefinition().buildLifecycle();
}
