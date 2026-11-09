#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_PREVIEW_RADIO_PREVIEW_PIPELINE_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_PREVIEW_RADIO_PREVIEW_PIPELINE_H

#include "../../graph/audio_graph.h"
#include "../../graph/audio_lifecycle.h"
#include "../../math/biquad.h"
#include "../../math/iq_fir_lowpass.h"
#include "../radio_am_ingress.h"
#include "../radio1938_constants.h"
#include "radio_preview_config.h"
#include "../models/radio_room.h"

#include <array>
#include <cstdint>
#include <vector>

struct Radio1938;

enum class RadioPreviewPassId : uint8_t {
  Warmup,
  BroadcastSource,
  ProgramFilter,
  ReceptionEnvironment,
};

struct RadioPreviewBlockControl {
  mutable bool programStartCached = false;
  mutable RadioPreviewPassId cachedProgramStartPass =
      RadioPreviewPassId::BroadcastSource;
  mutable size_t cachedProgramStartIndex = 0;
};

struct RadioPreviewSampleContext {
  const RadioPreviewBlockControl* block = nullptr;
  RadioAmReceptionSample* reception = nullptr;
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

struct RadioPreviewBroadcastSourceNode {
  static void init(RadioPreviewPipeline& preview, RadioPreviewInitContext&);
  static void reset(RadioPreviewPipeline& preview);
  static float run(RadioPreviewPipeline& preview,
                   float y,
                   RadioPreviewSampleContext& ctx);
};

struct RadioPreviewReceptionNode {
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
// Complex path gain of the unmodulated carrier at the first reception sample.
std::array<float, 2> radioPreviewInitialCarrierGain(const RadioPreviewPipeline& preview);
RadioPreviewLifecycle makeRadioPreviewLifecycle();

void beginRadioPreviewPipelineBlock(RadioPreviewPipeline& preview,
                                    uint32_t frames,
                                    RadioPreviewBlockControl& block);
float runRadioPreviewPipelineSample(RadioPreviewPipeline& preview,
                                    float x,
                                    RadioPreviewSampleContext& ctx,
                                    RadioPreviewPassId firstProgramPass);

struct RadioPreviewPipeline {
  struct RadioReceptionRuntime {
    uint32_t randomState = 0;
    float sampleRate = 0.0f;
    float sourceCarrierHz = 0.0f;
    float carrierPeak = 0.0f;
    double skyAmplitudeState = 0.0;
    double skyAmplitudePole = 0.0;
    double skyAmplitudeExcitation = 0.0;
    double skyDopplerState = 0.0;
    double skyDopplerPole = 0.0;
    double skyDopplerExcitation = 0.0;
    double skyDopplerBaseHz = 0.0;
    double skyOscCos = 1.0;
    double skyOscSin = 0.0;
    float skyDelaySamples = 0.0f;
    uint64_t intermittentWaitFrames = 0;
    uint64_t intermittentActiveFrames = 0;
    float intermittentEnvelope = 0.0f;
    float intermittentTarget = 0.0f;
    float intermittentAttackCoefficient = 0.0f;
    float intermittentReleaseCoefficient = 0.0f;
    float intermittentOffsetHz = 0.0f;
    float intermittentOscCos = 1.0f;
    float intermittentOscSin = 0.0f;
    float intermittentStepCos = 1.0f;
    float intermittentStepSin = 0.0f;
    struct GaussianStream {
      uint32_t state = 0;
      float spare = 0.0f;
      bool spareValid = false;
    };
    // Man-made noise and the Gaussian share of atmospheric noise.
    GaussianStream externalNoise;
    float externalNoiseSigma = 0.0f;
    // Sferics: a Bernoulli draw per sample (Poisson in the limit) and a
    // log-normal amplitude of fixed impulse area at every host rate.
    GaussianStream sferics;
    float sfericProbability = 0.0f;
    float sfericMedianVolts = 0.0f;
    float sfericLogSigma = 0.0f;
  } receptionRuntime;

  Radio1938* radio = nullptr;
  const RadioAmIngressConfig* ingress = nullptr;
  float sampleRate = 0.0f;
  float audioBandwidthHz = 0.0f;
  bool warmedUp = false;
  std::vector<float> warmupScratch;
  std::vector<float> filteredScratch;
  std::vector<RadioAmReceptionSample> receptionScratch;
  RadioRoomRenderer roomRenderer;
  Biquad programHp;
  Biquad programLp;
  IQFirLowpass programImageReject;
  float levelRidingEnvelope = 0.0f;
  float levelRidingFastEnvelope = 0.0f;
  float levelRidingFastCoefficient = 0.0f;
  float levelRidingGain = 1.0f;
  uint32_t levelRidingSettingFrames = 0;
  float levelRidingEnvelopeCoefficient = 0.0f;
  float levelRidingRaiseCoefficient = 0.0f;
  float levelRidingLowerCoefficient = 0.0f;
  float levelRidingSettingCoefficient = 0.0f;
  bool programLineEnabled = false;
  Biquad programLineHp;
  std::array<Biquad, 4> programLineLp;
  float broadcastCompressionThreshold = 0.0f;
  float broadcastEnvelope = 0.0f;
  float broadcastGain = 1.0f;
  float broadcastAttackCoefficient = 0.0f;
  float broadcastReleaseCoefficient = 0.0f;
  float broadcastNoiseAmplitude = 0.0f;
  uint32_t broadcastNoiseState = 0;

  using Graph = RadioPreviewGraph;
  using Lifecycle = RadioPreviewLifecycle;

  Graph graph = makeRadioPreviewGraph();
  Lifecycle lifecycle = makeRadioPreviewLifecycle();

  void initialize(Radio1938& radioRef,
                  const RadioAmIngressConfig& ingressConfig,
                  const RadioPreviewConfig& previewConfig,
                  float newSampleRate);
  void reserveBlockFrames(uint32_t frames);
  void reset();
  void runBlock(Radio1938& radioRef,
                float* samples,
                uint32_t frames,
                uint32_t channels);
};

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_PREVIEW_RADIO_PREVIEW_PIPELINE_H
