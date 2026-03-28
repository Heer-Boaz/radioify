#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_PIPELINE_TYPES_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_PIPELINE_TYPES_H

#include "../graph/audio_graph.h"
#include "../graph/audio_lifecycle.h"

#include <cstdint>

struct Radio1938;

enum class SourceInputMode : uint8_t {
  ComplexEnvelope,
  RealRf,
};

enum class PassId : uint8_t {
  Tuning,
  Input,
  AVC,
  AFC,
  ControlBus,
  InterferenceDerived,
  FrontEnd,
  Mixer,
  IFStrip,
  Demod,
  ReceiverInputNetwork,
  ReceiverCircuit,
  Tone,
  Power,
  Noise,
  Speaker,
  Cabinet,
  OutputScale,
  FinalLimiter,
  OutputClip,
};

struct RadioBlockControl {
  float tuneNorm = 0.0f;
};

struct RadioDerivedSampleParams {
  float demodIfNoiseAmp = 0.0f;
  float demodIfCrackleAmp = 0.0f;
  float demodIfCrackleRate = 0.0f;
  float noiseAmp = 0.0f;
  float crackleAmp = 0.0f;
  float crackleRate = 0.0f;
  float humAmp = 0.0f;
  bool humToneEnabled = true;
};

struct RadioSignalFrame {
  SourceInputMode mode = SourceInputMode::ComplexEnvelope;
  float i = 0.0f;
  float q = 0.0f;
  float detectorInputI = 0.0f;
  float detectorInputQ = 0.0f;

  void setRealRf(float x) {
    mode = SourceInputMode::RealRf;
    i = x;
    q = 0.0f;
  }

  void setComplexEnvelope(float inI, float inQ) {
    mode = SourceInputMode::ComplexEnvelope;
    i = inI;
    q = inQ;
  }
};

struct RadioSampleContext {
  const RadioBlockControl* block = nullptr;
  RadioDerivedSampleParams derived{};
  RadioSignalFrame signal{};
};

struct RadioInitContext {
  float tunedBw = 0.0f;
};

struct RadioTuningNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static void prepare(Radio1938& radio,
                      RadioBlockControl& block,
                      uint32_t frames);
};

struct RadioInputNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float run(Radio1938& radio, float y, RadioSampleContext& ctx);
};

struct RadioControlBusNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static void run(Radio1938& radio, RadioSampleContext& ctx);
};

struct RadioAVCNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static void run(Radio1938& radio, RadioSampleContext& ctx);
};

struct RadioAFCNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static void run(Radio1938& radio, RadioSampleContext& ctx);
};

struct RadioFrontEndNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float run(Radio1938& radio, float y, RadioSampleContext& ctx);
};

struct RadioMixerNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float run(Radio1938& radio, float y, RadioSampleContext& ctx);
};

struct RadioIFStripNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float run(Radio1938& radio, float y, RadioSampleContext& ctx);
};

struct RadioAMDetectorNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float run(Radio1938& radio, float y, RadioSampleContext& ctx);
};

struct RadioReceiverInputNetworkNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float run(Radio1938& radio, float y, RadioSampleContext& ctx);
};

struct RadioReceiverCircuitNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float run(Radio1938& radio, float y, RadioSampleContext& ctx);
};

struct RadioToneNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float run(Radio1938& radio, float y, RadioSampleContext& ctx);
};

struct RadioPowerNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float run(Radio1938& radio, float y, RadioSampleContext& ctx);
};

struct RadioInterferenceDerivedNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static void run(Radio1938& radio, RadioSampleContext& ctx);
};

struct RadioNoiseNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float run(Radio1938& radio, float y, RadioSampleContext& ctx);
};

struct RadioSpeakerNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float run(Radio1938& radio, float y, RadioSampleContext& ctx);
};

struct RadioCabinetNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float run(Radio1938& radio, float y, RadioSampleContext& ctx);
};

struct RadioOutputScaleNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float run(Radio1938& radio, float y, RadioSampleContext& ctx);
};

struct RadioFinalLimiterNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float run(Radio1938& radio, float y, RadioSampleContext& ctx);
};

struct RadioOutputClipNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float run(Radio1938& radio, float y, RadioSampleContext& ctx);
};

using RadioGraph =
    AudioGraphRuntime<Radio1938, RadioBlockControl, RadioSampleContext, PassId>;
using RadioLifecycle = AudioLifecycleRuntime<Radio1938, RadioInitContext, PassId>;

RadioGraph makeRadioGraph();
RadioLifecycle makeRadioLifecycle();

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_PIPELINE_TYPES_H
