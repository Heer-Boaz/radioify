#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_PREVIEW_RADIO_PREVIEW_PIPELINE_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_PREVIEW_RADIO_PREVIEW_PIPELINE_H

#include "../../graph/audio_graph.h"
#include "../../graph/audio_lifecycle.h"
#include "../../math/biquad.h"
#include "../radio_am_ingress.h"
#include "../radio1938_constants.h"
#include "radio_preview_config.h"

#include <cstdint>
#include <vector>

struct Radio1938;

enum class RadioPreviewPassId : uint8_t {
  Warmup,
  ProgramFilter,
};

struct RadioPreviewBlockControl {};

struct RadioPreviewSampleContext {
  const RadioPreviewBlockControl* block = nullptr;
};

struct RadioPreviewInitContext {};

struct RadioPreviewPipeline;

struct RadioPreviewWarmupNode {
  static void init(RadioPreviewPipeline& preview, RadioPreviewInitContext&);
  static void reset(RadioPreviewPipeline& preview);
  static void prepare(RadioPreviewPipeline& preview,
                      RadioPreviewBlockControl& block,
                      uint32_t frames);
};

struct RadioPreviewProgramFilterNode {
  static void init(RadioPreviewPipeline& preview, RadioPreviewInitContext&);
  static void reset(RadioPreviewPipeline& preview);
  static float run(RadioPreviewPipeline& preview,
                   float y,
                   RadioPreviewSampleContext& ctx);
};

using RadioPreviewGraph =
    AudioGraphRuntime<RadioPreviewPipeline,
                      RadioPreviewBlockControl,
                      RadioPreviewSampleContext,
                      RadioPreviewPassId>;
using RadioPreviewLifecycle =
    AudioLifecycleRuntime<RadioPreviewPipeline,
                          RadioPreviewInitContext,
                          RadioPreviewPassId>;

RadioPreviewGraph makeRadioPreviewGraph();
RadioPreviewLifecycle makeRadioPreviewLifecycle();

void beginRadioPreviewPipelineBlock(RadioPreviewPipeline& preview,
                                    uint32_t frames,
                                    RadioPreviewBlockControl& block);
float runRadioPreviewPipelineSample(RadioPreviewPipeline& preview,
                                    float x,
                                    RadioPreviewSampleContext& ctx,
                                    RadioPreviewPassId firstProgramPass);

struct RadioPreviewPipeline {
  Radio1938* radio = nullptr;
  const RadioAmIngressConfig* ingress = nullptr;
  const RadioPreviewConfig* config = nullptr;
  float sampleRate = 0.0f;
  bool warmedUp = false;
  std::vector<float> warmupScratch;
  std::vector<float> filteredScratch;
  Biquad programHp;
  Biquad programLp1;
  Biquad programLp2;

  using Graph = RadioPreviewGraph;
  using Lifecycle = RadioPreviewLifecycle;

  Graph graph = makeRadioPreviewGraph();
  Lifecycle lifecycle = makeRadioPreviewLifecycle();

  void initialize(Radio1938& radioRef,
                  const RadioAmIngressConfig& ingressConfig,
                  const RadioPreviewConfig& previewConfig,
                  float newSampleRate);
  void reset();
  void runBlock(Radio1938& radioRef,
                float* samples,
                uint32_t frames,
                uint32_t channels);
};

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_PREVIEW_RADIO_PREVIEW_PIPELINE_H
