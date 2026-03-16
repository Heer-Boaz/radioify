#ifndef RADIO_H
#define RADIO_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string_view>
#include <vector>

inline constexpr float kRadioPi = 3.1415926535f;
inline constexpr float kRadioTwoPi = 6.283185307f;
inline constexpr float kRadioBiquadQ = 0.707f;
inline constexpr float kRadioLinDbFloor = 1e-12f;
inline constexpr float kRadioDiodeColorLeak = 0.06f;
inline constexpr float kRadioDiodeColorCurve = 0.35f;
inline constexpr float kRadioSoftClipThresholdDefault = 0.98f;

struct Biquad {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
  float z1 = 0.0f;
  float z2 = 0.0f;

  float process(float x);
  void reset();
  void setLowpass(float sampleRate, float freq, float q);
  void setHighpass(float sampleRate, float freq, float q);
  void setPeaking(float sampleRate, float freq, float q, float gainDb);
};

struct Compressor {
  float fs = 48000.0f;
  float thresholdDb = -24.0f;
  float ratio = 4.0f;
  float attackMs = 12.0f;
  float releaseMs = 180.0f;

  float env = 0.0f;
  float gainDb = 0.0f;
  float atkCoeff = 0.0f;
  float relCoeff = 0.0f;
  float gainAtkCoeff = 0.0f;
  float gainRelCoeff = 0.0f;

  void setFs(float newFs);
  void setTimes(float aMs, float rMs);
  void reset();
  float process(float x);
};

struct Saturator {
  float drive = 1.35f;
  float mix = 0.45f;

  float process(float x);
};

struct NoiseInput {
  float programSample = 0.0f;
  float noiseAmp = 0.0f;
  float crackleAmp = 0.0f;
  float crackleRate = 0.0f;
  float lightningAmp = 0.0f;
  float lightningRate = 0.0f;
  float motorAmp = 0.0f;
  float motorRate = 0.0f;
  float motorBuzzHz = 0.0f;
  float humAmp = 0.0f;
  bool humToneEnabled = true;
};

struct NoiseHum {
  std::mt19937 rng{0x1938u};
  std::uniform_real_distribution<float> dist{-1.0f, 1.0f};
  std::uniform_real_distribution<float> dist01{0.0f, 1.0f};
  Biquad hp;
  Biquad lp;
  Biquad crackleHp;
  Biquad crackleLp;
  Biquad lightningHp;
  Biquad lightningLp;
  Biquad motorHp;
  Biquad motorLp;
  float fs = 48000.0f;
  float noiseHpHz = 220.0f; // 500.0f
  float noiseLpHz = 5500.0f;
  float humHz = 50.0f;
  float humPhase = 0.0f;
  float scEnv = 0.0f;
  float scAtk = 0.0f;
  float scRel = 0.0f;
  float crackleEnv = 0.0f;
  float crackleDecay = 0.0f;
  float lightningEnv = 0.0f;
  float lightningDecay = 0.0f;
  float motorEnv = 0.0f;
  float motorDecay = 0.0f;
  float motorPhase = 0.0f;
  float pinkFast = 0.0f;
  float pinkSlow = 0.0f;
  float brown = 0.0f;
  float hissDrift = 0.0f;
  float hissDriftSlow = 0.0f;
  float filterQ = kRadioBiquadQ;
  float lightningHpHz = 200.0f;
  float lightningLpScale = 1.1f;
  float lightningLpMinGapHz = 500.0f;
  float motorHpHz = 800.0f;
  float motorLpHz = 3600.0f;
  float scAttackMs = 10.0f;
  float scReleaseMs = 320.0f;
  float crackleDecayMs = 12.0f;
  float lightningDecayMs = 180.0f;
  float motorDecayMs = 70.0f;
  float sidechainMaskRef = 0.18f;
  float hissMaskDepth = 0.12f;
  float burstMaskDepth = 0.04f;
  float pinkFastPole = 0.985f;
  float pinkSlowPole = 0.9975f;
  float brownStep = 0.0015f;
  float hissDriftPole = 0.9995f;
  float hissDriftNoise = 0.0005f;
  float hissDriftSlowPole = 0.99985f;
  float hissDriftSlowNoise = 0.00015f;
  float whiteMix = 0.82f;
  float pinkFastMix = 0.22f;
  float pinkDifferenceMix = 0.12f;
  float pinkFastSubtract = 0.5f;
  float brownMix = 0.08f;
  float hissBase = 0.88f;
  float hissDriftDepth = 0.20f;
  float hissDriftSlowMix = 0.06f;
  float noiseScaleRef = 0.015f;
  float noiseScaleMax = 3.0f;
  float motorBuzzBase = 0.65f;
  float motorBuzzDepth = 0.35f;
  float humSecondHarmonicMix = 0.35f;

  void setFs(float newFs, float noiseBwHz);
  void reset();
  float process(const NoiseInput& in);
};

struct AMDetectorSampleInput {
  float signal = 0.0f;
  float ifNoiseAmp = 0.0f;
};

struct AMDetector {
  float fs = 48000.0f;
  float bwHz = 4800.0f;
  float carrierHz = 12000.0f;
  float tuneOffsetHz = 0.0f;
  float modIndex = 0.80f;
  float carrierGain = 0.40f;
  float diodeDrop = 0.008f;
  float detGain = 2.70f;

  enum class Mode {
    Envelope,
    Iq,
  };
  Mode mode = Mode::Iq;

  std::mt19937 rng{0x1942u};
  std::uniform_real_distribution<float> dist{-1.0f, 1.0f};

  Biquad ifHpIn;
  Biquad ifHpOut;
  Biquad ifLpIn;
  Biquad ifLpOut;
  Biquad detAudioLpIn;
  Biquad detAudioLpOut;
  Biquad detAudioRippleLp;
  Biquad detQuadratureLpIn;
  Biquad detQuadratureLpOut;
  Biquad detQuadratureRippleLp;

  float phase = 0.0f;
  float rxPhase = 0.0f;
  float agcEnv = 0.0f;
  float agcGainDb = 0.0f;
  float agcTargetDb = -18.0f;
  float agcMaxGainDb = 12.0f;
  float agcMinGainDb = -4.0f;
  float agcAtk = 0.0f;
  float agcRel = 0.0f;
  float agcGainAtk = 0.0f;
  float agcGainRel = 0.0f;

  float detectorCap = 0.0f;
  float detChargeCoeff = 0.0f;
  float detReleaseCoeff = 0.0f;
  float avcCap = 0.0f;
  float avcChargeCoeff = 0.0f;
  float avcReleaseCoeff = 0.0f;
  float dcEnv = 0.0f;
  float dcCoeff = 0.0f;
  float initCarrierHz = 9000.0f;
  float initCarrierMaxFraction = 0.24f;
  float agcAttackMs = 95.0f;
  float agcReleaseMs = 2400.0f;
  float agcGainAttackMs = 70.0f;
  float agcGainReleaseMs = 3100.0f;
  float detChargeMs = 0.18f;
  float detReleaseMs = 1.60f;
  float avcChargeMs = 24.0f;
  float avcReleaseMs = 2600.0f;
  float dcMs = 450.0f;
  float ifHalfBwScale = 0.95f;
  float ifHalfBwMinHz = 1500.0f;
  float ifHalfBwMaxFraction = 0.2f;
  float ifCenterMinHz = 1000.0f;
  float ifGuardFraction = 0.45f;
  float ifMaxFraction = 0.48f;
  float ifSpanMinHz = 800.0f;
  float ifSkewPositiveExpand = 0.10f;
  float ifSkewNegativeShrink = 0.18f;
  float ifSkewPositiveShrink = 0.18f;
  float ifSkewNegativeExpand = 0.10f;
  float ifSpanClampMinHz = 1200.0f;
  float ifSpanClampMaxFraction = 0.22f;
  float detLpScale = 1.15f;
  float detLpMinHz = 2800.0f;
  float mistuneNormDenomScale = 0.5f;
  float detectorCompression = 0.85f;
  float detReleaseMistuneMix = 0.45f;
  float avcImpulseBase = 0.55f;
  float avcImpulseMistune = 0.20f;
  float consonantPullScale = 8.0f;
  float consonantPullMaxDb = 1.2f;
  float mistuneGainPenaltyDb = 0.8f;
  float iqLevelComp = 2.0f;
  float envelopeRippleBase = 0.08f;
  float envelopeRippleMistune = 0.12f;
  float overloadThreshold = 0.18f;
  float overloadRange = 0.24f;
  float overloadImpulseScale = 1.8f;
  float negLimitBase = 0.34f;
  float negLimitOverloadDepth = 0.12f;
  float negLimitMistuneDepth = 0.04f;
  float negLimitMin = 0.12f;
  float posLimitBase = 0.92f;
  float posLimitOverloadDepth = 0.06f;
  float posLimitGuard = 0.15f;
  float negOverloadSoftness = 4.0f;
  float posOverloadSoftness = 1.5f;
  float modulationClamp = 0.98f;

  void init(float newFs, float newBw, float newTuneHz = 0.0f);
  void setBandwidth(float newBw, float newTuneHz = 0.0f);
  void reset();
  float process(const AMDetectorSampleInput& in);
};

struct SpeakerSim {
  Biquad boxResBass;
  Biquad boxResLowMid;
  Biquad cabNasal;
  Biquad panelRes;
  Biquad coneDip;
  Biquad paperPeak;
  Biquad hornPeak;
  Biquad backWaveHp;
  Biquad backWaveLp;
  float drive = 1.05f;
  float mix = 0.24f;
  float limit = 0.93f;
  float asymBias = 0.028f;
  float backWaveMix = 0.10f;
  int backWaveIndex = 0;
  std::vector<float> backWaveBuf;
  float filterQ = kRadioBiquadQ;
  float boxResBassHz = 155.0f;
  float boxResBassQ = 0.68f;
  float boxResBassGainDb = 1.15f;
  float boxResLowMidHz = 430.0f;
  float boxResLowMidQ = 0.82f;
  float boxResLowMidGainDb = 0.45f;
  float cabNasalHz = 820.0f;
  float cabNasalQ = 1.10f;
  float cabNasalGainDb = -0.65f;
  float panelResHz = 610.0f;
  float panelResQ = 1.15f;
  float panelResGainDb = 0.30f;
  float hornPeakHz = 1680.0f;
  float hornPeakQ = 0.98f;
  float hornPeakGainDb = 0.95f;
  float paperPeakHz = 2280.0f;
  float paperPeakQ = 1.25f;
  float paperPeakGainDb = 0.40f;
  float coneDipHz = 3180.0f;
  float coneDipQ = 0.95f;
  float coneDipGainDb = -1.75f;
  float backWaveHpHz = 165.0f;
  float backWaveLpHz = 920.0f;
  float backWaveDelayMs = 0.9f;

  void init(float fs);
  void reset();
  float process(float x, bool& clipped);
};

struct Radio1938;

enum class StageId : uint8_t {
  Tuning,
  Input,
  Reception,
  InterferenceDerived,
  FrontEnd,
  Multipath,
  Detune,
  Adjacent,
  Demod,
  Tone,
  Power,
  Noise,
  Heterodyne,
  Speaker,
  Room,
  OutputClip,
};

struct RadioBlockControl {
  float sampleRate = 48000.0f;
  float tuneNorm = 0.0f;
  float offT = 0.0f;
  float offHz = 0.0f;
  float cosmeticOffT = 0.0f;
};

struct RadioSampleControl {
  float receptionGain = 1.0f;
  float noiseScale = 1.0f;
};

struct RadioDerivedSampleParams {
  float demodIfNoiseAmp = 0.0f;
  float noiseAmp = 0.0f;
  float crackleAmp = 0.0f;
  float crackleRate = 0.0f;
  float lightningAmp = 0.0f;
  float lightningRate = 0.0f;
  float motorAmp = 0.0f;
  float motorRate = 0.0f;
  float motorBuzzHz = 0.0f;
  float humAmp = 0.0f;
  float quieting = 1.0f;
  bool humToneEnabled = true;
};

struct RadioSampleContext {
  const RadioBlockControl* block = nullptr;
  RadioSampleControl control{};
  RadioDerivedSampleParams derived{};
};

struct RadioInitContext {
  float tunedBw = 0.0f;
};

struct RadioTuningNode {
  static float applyFilters(Radio1938& radio, float tuneHz, float bwHz);
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static void prepare(Radio1938& radio,
                      RadioBlockControl& block,
                      uint32_t frames);
};

struct RadioInputNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float process(Radio1938& radio,
                       float y,
                       const RadioSampleContext& ctx);
};

struct RadioReceptionControlNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static void update(Radio1938& radio, RadioSampleContext& ctx);
};

struct RadioFrontEndNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float process(Radio1938& radio,
                       float y,
                       const RadioSampleContext& ctx);
};

struct RadioMultipathNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float process(Radio1938& radio,
                       float y,
                       const RadioSampleContext& ctx);
};

struct RadioDetuneNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float process(Radio1938& radio,
                       float y,
                       const RadioSampleContext& ctx);
};

struct RadioAdjacentNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float process(Radio1938& radio,
                       float y,
                       const RadioSampleContext& ctx);
};

struct RadioDemodNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float process(Radio1938& radio,
                       float y,
                       const RadioSampleContext& ctx);
};

struct RadioToneNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float process(Radio1938& radio,
                       float y,
                       const RadioSampleContext& ctx);
};

struct RadioPowerNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float process(Radio1938& radio,
                       float y,
                       const RadioSampleContext& ctx);
};

struct RadioInterferenceDerivedNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static void update(Radio1938& radio, RadioSampleContext& ctx);
};

struct RadioNoiseNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float process(Radio1938& radio,
                       float y,
                       const RadioSampleContext& ctx);
};

struct RadioHeterodyneNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float process(Radio1938& radio,
                       float y,
                       const RadioSampleContext& ctx);
};

struct RadioSpeakerNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float process(Radio1938& radio,
                       float y,
                       const RadioSampleContext& ctx);
};

struct RadioRoomNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float process(Radio1938& radio,
                       float y,
                       const RadioSampleContext& ctx);
};

struct RadioOutputClipNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float process(Radio1938& radio,
                       float y,
                       const RadioSampleContext& ctx);
};

struct Radio1938 {
  enum class Preset {
    Mid30sHeroic,
    Mid30sDocumentary,
  };

  float sampleRate = 48000.0f;
  int channels = 1;
  float bwHz = 4800.0f;
  float noiseWeight = 0.012f;
  float makeupGain = 1.0f; // baseline unity gain
  Preset preset = Preset::Mid30sHeroic;
  bool initialized = false;

  using BlockPrepareFn = void (*)(Radio1938&, RadioBlockControl&, uint32_t);
  using SampleControlFn = void (*)(Radio1938&, RadioSampleContext&);
  using SampleProcessFn = float (*)(Radio1938&, float, const RadioSampleContext&);
  using InitFn = void (*)(Radio1938&, RadioInitContext&);
  using ResetFn = void (*)(Radio1938&);

  struct BlockStep {
    StageId id = StageId::Tuning;
    std::string_view name{};
    bool enabled = true;
    BlockPrepareFn prepare = nullptr;
  };

  struct SampleControlStep {
    StageId id = StageId::Reception;
    std::string_view name{};
    bool enabled = true;
    SampleControlFn update = nullptr;
  };

  struct ProgramPathStep {
    StageId id = StageId::Input;
    std::string_view name{};
    bool enabled = true;
    SampleProcessFn process = nullptr;
  };

  struct PresentationPathStep {
    StageId id = StageId::Noise;
    std::string_view name{};
    bool enabled = true;
    SampleProcessFn process = nullptr;
  };

  struct ConfigureStep {
    StageId id = StageId::Input;
    std::string_view name{};
    InitFn init = nullptr;
  };

  struct AllocateStep {
    StageId id = StageId::Input;
    std::string_view name{};
    InitFn init = nullptr;
  };

  struct InitializeDependentStateStep {
    StageId id = StageId::Input;
    std::string_view name{};
    InitFn init = nullptr;
  };

  struct ResetStep {
    StageId id = StageId::Input;
    std::string_view name{};
    ResetFn reset = nullptr;
  };

  struct RadioExecutionGraph {
    bool bypass = false;

    std::array<BlockStep, 1> blockSteps{{
        {StageId::Tuning, "Tuning", true, &RadioTuningNode::prepare},
    }};

    std::array<SampleControlStep, 2> sampleControlSteps{{
        {StageId::Reception, "Reception", true, &RadioReceptionControlNode::update},
        {StageId::InterferenceDerived, "InterferenceDerived", true,
         &RadioInterferenceDerivedNode::update},
    }};

    std::array<ProgramPathStep, 8> programPathSteps{{
        {StageId::Input, "Input", true, &RadioInputNode::process},
        {StageId::FrontEnd, "FrontEnd", true, &RadioFrontEndNode::process},
        {StageId::Multipath, "Multipath", true, &RadioMultipathNode::process},
        {StageId::Detune, "Detune", true, &RadioDetuneNode::process},
        {StageId::Adjacent, "Adjacent", true, &RadioAdjacentNode::process},
        {StageId::Demod, "Demod", true, &RadioDemodNode::process},
        {StageId::Tone, "Tone", true, &RadioToneNode::process},
        {StageId::Power, "Power", true, &RadioPowerNode::process},
    }};

    std::array<PresentationPathStep, 5> presentationPathSteps{{
        {StageId::Noise, "Noise", true, &RadioNoiseNode::process},
        {StageId::Heterodyne, "Heterodyne", true, &RadioHeterodyneNode::process},
        {StageId::Speaker, "Speaker", true, &RadioSpeakerNode::process},
        {StageId::Room, "Room", true, &RadioRoomNode::process},
        {StageId::OutputClip, "OutputClip", true, &RadioOutputClipNode::process},
    }};

    BlockStep* findBlock(StageId id);
    const BlockStep* findBlock(StageId id) const;
    SampleControlStep* findSampleControl(StageId id);
    const SampleControlStep* findSampleControl(StageId id) const;
    ProgramPathStep* findProgramPath(StageId id);
    const ProgramPathStep* findProgramPath(StageId id) const;
    PresentationPathStep* findPresentationPath(StageId id);
    const PresentationPathStep* findPresentationPath(StageId id) const;
    bool isEnabled(StageId id) const;
    void setEnabled(StageId id, bool value);
  } graph;

  struct RadioLifecycle {
    std::array<ConfigureStep, 8> configureSteps{{
        {StageId::Input, "Input", &RadioInputNode::init},
        {StageId::Reception, "Reception", &RadioReceptionControlNode::init},
        {StageId::FrontEnd, "FrontEnd", &RadioFrontEndNode::init},
        {StageId::Adjacent, "Adjacent", &RadioAdjacentNode::init},
        {StageId::Tone, "Tone", &RadioToneNode::init},
        {StageId::Power, "Power", &RadioPowerNode::init},
        {StageId::Speaker, "Speaker", &RadioSpeakerNode::init},
        {StageId::OutputClip, "OutputClip", &RadioOutputClipNode::init},
    }};

    std::array<AllocateStep, 3> allocateSteps{{
        {StageId::Detune, "Detune", &RadioDetuneNode::init},
        {StageId::Multipath, "Multipath", &RadioMultipathNode::init},
        {StageId::Room, "Room", &RadioRoomNode::init},
    }};

    std::array<InitializeDependentStateStep, 5> initializeDependentStateSteps{{
        {StageId::Tuning, "Tuning", &RadioTuningNode::init},
        {StageId::Demod, "Demod", &RadioDemodNode::init},
        {StageId::Noise, "Noise", &RadioNoiseNode::init},
        {StageId::InterferenceDerived, "InterferenceDerived",
         &RadioInterferenceDerivedNode::init},
        {StageId::Heterodyne, "Heterodyne", &RadioHeterodyneNode::init},
    }};

    std::array<ResetStep, 13> resetSteps{{
        {StageId::Reception, "Reception", &RadioReceptionControlNode::reset},
        {StageId::Detune, "Detune", &RadioDetuneNode::reset},
        {StageId::Adjacent, "Adjacent", &RadioAdjacentNode::reset},
        {StageId::Heterodyne, "Heterodyne", &RadioHeterodyneNode::reset},
        {StageId::Multipath, "Multipath", &RadioMultipathNode::reset},
        {StageId::Power, "Power", &RadioPowerNode::reset},
        {StageId::Input, "Input", &RadioInputNode::reset},
        {StageId::Room, "Room", &RadioRoomNode::reset},
        {StageId::FrontEnd, "FrontEnd", &RadioFrontEndNode::reset},
        {StageId::Tone, "Tone", &RadioToneNode::reset},
        {StageId::Demod, "Demod", &RadioDemodNode::reset},
        {StageId::Speaker, "Speaker", &RadioSpeakerNode::reset},
        {StageId::Noise, "Noise", &RadioNoiseNode::reset},
    }};

    void configure(Radio1938& radio, RadioInitContext& initCtx) const;
    void allocate(Radio1938& radio, RadioInitContext& initCtx) const;
    void initializeDependentState(Radio1938& radio,
                                  RadioInitContext& initCtx) const;
    void resetRuntime(Radio1938& radio) const;
  } lifecycle;

  struct RadioDiagnostics {
    bool anyClip = false;
    bool powerClip = false;
    bool speakerClip = false;
    bool outputClip = false;

    void reset() {
      anyClip = false;
      powerClip = false;
      speakerClip = false;
      outputClip = false;
    }

    void markPowerClip() {
      anyClip = true;
      powerClip = true;
    }

    void markSpeakerClip() {
      anyClip = true;
      speakerClip = true;
    }

    void markOutputClip() {
      anyClip = true;
      outputClip = true;
    }
  } diagnostics;

  struct RadioRuntimeObservables {
    float powerSag = 0.0f;

    void reset() { powerSag = 0.0f; }
  } runtime;

  struct GlobalTuning {
    float oversampleFactor = 2.0f;
    float ifNoiseMix = 0.22f;
    float postNoiseMix = 0.14f;
    float noiseFloorAmp = 0.0022f;
    float compMakeupGain = 1.18f;
    float inputPad = 0.76f;
    bool enableAutoLevel = true;
    float autoTargetDb = -21.0f;
    float autoMaxBoostDb = 4.0f;
    float satClipDelta = 0.03f;
    float satClipMinLevel = 0.70f;
    float outputClipThreshold = 0.995f;
    float oversampleCutoffFraction = 0.45f;
  } globals;

  struct TuningNodeState {
    float tuneOffsetHz = 0.0f;
    float tuneOffsetNorm = 0.0f;
    float tunedBw = 0.0f;
    float tuneAppliedHz = 0.0f;
    float bwAppliedHz = 0.0f;
    float tuneSmoothedHz = 0.0f;
    float bwSmoothedHz = 0.0f;
    float tuneTiltExtra = 0.14f;
    float safeBwMinHz = 4200.0f;
    float safeBwMaxHz = 5600.0f;
    float tunedBwMistuneDepth = 0.28f;
    float tunedBwMinHz = 3000.0f;
    float preBwScale = 1.08f;
    float postBwScale = 1.18f;
    float rippleShiftScale = 0.18f;
    float ifRippleLowHz = 950.0f;
    float ifRippleLowMinHz = 500.0f;
    float ifRippleLowQ = 0.9f;
    float ifRippleLowGainDb = 0.35f;
    float ifRippleHighHz = 2300.0f;
    float ifRippleHighMinHz = 800.0f;
    float ifRippleHighQ = 1.1f;
    float ifRippleHighGainDb = -0.45f;
    float tiltDetuneDepth = 0.12f;
    float tiltHz = 1700.0f;
    float tiltTuneScale = 0.15f;
    float tiltMinHz = 800.0f;
    float adjLpScale = 0.78f;
    float adjLpMinHz = 2200.0f;
    float adjLpMaxScale = 0.90f;
    float smoothTau = 0.05f;
    float updateEps = 0.25f;
    float mistuneCosmeticStart = 0.18f;
    float mistuneCosmeticRange = 0.82f;
  } tuning;

  struct InputNodeState {
    float autoEnv = 0.0f;
    float autoGainDb = 0.0f;
    float autoEnvAtk = 0.0f;
    float autoEnvRel = 0.0f;
    float autoGainAtk = 0.0f;
    float autoGainRel = 0.0f;
    float autoEnvAttackMs = 8.0f;
    float autoEnvReleaseMs = 220.0f;
    float autoGainAttackMs = 140.0f;
    float autoGainReleaseMs = 1200.0f;
  } input;

  struct ReceptionNodeState {
    float fadePhaseFast = 0.0f;
    float fadePhaseSlow = 0.0f;
    float noisePhaseFast = 0.0f;
    float noisePhaseSlow = 0.0f;
    float fadeRateFast = 0.08f;
    float fadeRateSlow = 0.05f;
    float noiseRateFast = 0.04f;
    float noiseRateSlow = 0.031f;
    float fadeDepth = 0.05f;
    float noiseDepth = 0.20f;
    float lfoMixA = 0.6f;
    float lfoMixB = 0.4f;
    float fadeMin = 0.65f;
    float fadeMax = 1.35f;
    float receptionBase = 0.6f;
    float receptionOffTScale = 0.6f;
    float receptionFadeScale = 0.4f;
    float noiseScaleMin = 0.3f;
    float noiseScaleMax = 1.6f;
  } reception;

  struct FrontEndNodeState {
    float ifTiltMix = 0.10f;
    float inputHpHz = 115.0f;
    Biquad hpf;
    Biquad preLpfIn;
    Biquad preLpfOut;
    Biquad ifRippleLow;
    Biquad ifRippleHigh;
    Biquad ifTiltLp;
  } frontEnd;

  struct MultipathNodeState {
    float mixPhase = 0.0f;
    float blendPhase = 0.0f;
    float delayPhase = 0.0f;
    float mixRate = 0.035f;
    float blendRate = 0.027f;
    float delayRate = 0.041f;
    float mix = 0.12f;
    float depth = 0.08f;
    float delayMs = 5.5f;
    float delayModMs = 1.5f;
    float tiltMix = 0.35f;
    float tiltLpHz = 1800.0f;
    int minBufferSamples = 8;
    int bufferGuardSamples = 4;
    float maxMix = 0.35f;
    float lfoMixA = 0.6f;
    float lfoMixB = 0.4f;
    float directMixDepth = 0.5f;
    std::vector<float> buf;
    int index = 0;
    Biquad tiltLp;
  } multipath;

  struct DetuneNodeState {
    float lfoPhaseFast = 0.0f;
    float lfoPhaseSlow = 0.0f;
    float lfoRateFast = 0.13f;
    float lfoRateSlow = 0.09f;
    float depth = 0.0f;
    float baseDelay = 2.0f;
    int bufferSamples = 64;
    float minDelay = 0.25f;
    float lfoMixA = 0.6f;
    float lfoMixB = 0.4f;
    std::vector<float> buf;
    int index = 0;
  } detune;

  struct AdjacentNodeState {
    float phase = 0.0f;
    float beatHz = 980.0f;
    float modDepth = 0.12f;
    float mix = 0.010f;
    float drive = 1.40f;
    float splatterMix = 0.14f;
    float env = 0.0f;
    float atk = 0.0f;
    float rel = 0.0f;
    float target = 0.18f;
    float minGain = 0.25f;
    float maxGain = 4.0f;
    float hpHz = 250.0f;
    float attackMs = 120.0f;
    float releaseMs = 900.0f;
    float maxMix = 0.20f;
    Biquad hp;
    Biquad lp;
  } adjacent;

  struct DemodNodeState {
    AMDetector am;
    float diodeColorDrop = 0.004f;
  } demod;

  struct ToneNodeState {
    float env = 0.0f;
    float atk = 0.0f;
    float rel = 0.0f;
    float midBoostHz = 1250.0f;
    float midBoostQ = 0.8f;
    float midBoostGainDb = 1.4f;
    float lowMidDipHz = 340.0f;
    float lowMidDipQ = 1.0f;
    float lowMidDipGainDb = -1.0f;
    float presBoostHz = 3300.0f;
    float presBoostQ = 0.9f;
    float presBoostGainDb = -0.8f;
    float lowLpHz = 430.0f;
    float highHpHz = 1750.0f;
    float compThresholdDb = -12.0f;
    float compRatio = 1.35f;
    float compAttackMs = 140.0f;
    float compReleaseMs = 1100.0f;
    float attackMs = 22.0f;
    float releaseMs = 320.0f;
    float loudnessEnvStart = 0.010f;
    float loudnessEnvRange = 0.11f;
    float lowBaseGain = 0.80f;
    float lowGainDepth = 0.18f;
    float midBaseGain = 0.90f;
    float midGainDepth = 0.18f;
    float highBaseGain = 0.46f;
    float highGainDepth = 0.34f;
    Biquad midBoost;
    Biquad lowMidDip;
    Biquad presBoost;
    Biquad lowLp;
    Biquad highHp;
    Compressor comp;
  } tone;

  struct PowerNodeState {
    float sagEnv = 0.0f;
    float sagAtk = 0.0f;
    float sagRel = 0.0f;
    float sagStart = 0.06f;
    float sagEnd = 0.22f;
    float sagDepth = 0.04f;
    float env = 0.0f;
    float atk = 0.0f;
    float rel = 0.0f;
    float rectifierPhase = 0.0f;
    float subharmonicPhase = 0.0f;
    float rippleDepth = 0.010f;
    float biasDepth = 0.006f;
    float satOsPrev = 0.0f;
    float satDrive = 1.12f;
    float satMix = 0.24f;
    float sagAttackMs = 60.0f;
    float sagReleaseMs = 900.0f;
    float attackMs = 25.0f;
    float releaseMs = 520.0f;
    float powerEnvStart = 0.05f;
    float powerEnvRange = 0.25f;
    float rectifierMinHz = 80.0f;
    float rectifierSubharmonic = 0.5f;
    float rippleSecondHarmonicMix = 0.28f;
    float rippleSubharmonicMix = 0.15f;
    float gainSagPerPower = 0.018f;
    float rippleGainBase = 0.35f;
    float rippleGainDepth = 0.65f;
    float gainMin = 0.88f;
    float gainMax = 1.04f;
    float biasBase = 0.2f;
    float biasPowerDepth = 0.8f;
    Biquad postLpf;
    Biquad satOsLpIn;
    Biquad satOsLpOut;
    Saturator sat;
  } power;

  struct HeterodyneNodeState {
    bool enabled = true;
    float phase = 0.0f;
    float driftPhase = 0.0f;
    float driftHz = 0.06f;
    float gateStart = 0.015f;
    float gateEnd = 0.05f;
    float depth = 0.00035f;
    float heteroBaseScale = 0.0f;
    float noiseWeightRef = 0.015f;
    float heteroBaseScaleMin = 0.10f;
    float heteroBaseScaleMax = 0.85f;
    float gateHz = 180.0f;
    float maxHz = 2500.0f;
    float driftDepth = 0.12f;
    float quietNoiseBase = 0.6f;
    float quietNoiseDepth = 0.4f;
  } heterodyne;

  struct NoiseConfig {
    bool enableHumTone = true;
    float humHzDefault = 50.0f;
    float noiseWeightRef = 0.015f;
    float noiseWeightScaleMax = 2.0f;
    float humAmpScale = 0.0018f;
    float crackleAmpScale = 0.008f;
    float lightningAmpScale = 0.022f;
    float motorAmpScale = 0.0045f;
    float crackleRateScale = 0.50f;
    float lightningRateScale = 0.006f;
    float motorRateScale = 0.06f;
    float motorBuzzHz = 18.0f;
  } noiseConfig;

  struct NoiseDerivedState {
    float baseNoiseAmp = 0.0f;
    float baseCrackleAmp = 0.0f;
    float baseLightningAmp = 0.0f;
    float baseMotorAmp = 0.0f;
    float baseHumAmp = 0.0f;
    float crackleRate = 0.0f;
    float lightningRate = 0.0f;
    float motorRate = 0.0f;
  } noiseDerived;

  struct NoiseRuntimeState {
    NoiseHum hum;
  } noiseRuntime;

  struct SpeakerStageState {
    float osPrev = 0.0f;
    float drive = 0.72f;
    Biquad postLpf;
    Biquad osLpIn;
    Biquad osLpOut;
    SpeakerSim speaker;
  } speakerStage;

  struct RoomNodeState {
    bool enableEarlyReflections = true;
    bool enableTail = false;
    float mix = 0.025f;
    float lpHz = 1800.0f;
    int index = 0;
    int delaySamples = 0;
    std::vector<float> buf;
    std::vector<int> tapSamples;
    std::vector<float> tapGains;
    float tailMix = 0.0f;
    float tailFeedback = 0.28f;
    float tailMs = 45.0f;
    float tailLpHz = 1600.0f;
    int tailIndex = 0;
    std::vector<float> tailBuf;
    std::array<float, 5> tapMs{{6.0f, 10.5f, 16.0f, 23.0f, 31.0f}};
    std::array<float, 5> tapGain{{0.18f, 0.14f, 0.10f, 0.07f, 0.05f}};
    float baseDelayMs = 12.0f;
    int tailMinSamples = 4;
    Biquad lp;
    Biquad tailLp;
  } room;

  struct OutputNodeState {
    float clipOsPrev = 0.0f;
    Biquad clipOsLpIn;
    Biquad clipOsLpOut;
  } output;

  static std::string_view presetName(Preset preset);
  bool applyPreset(std::string_view presetName);
  void applyPreset(Preset preset);
  void init(int ch, float sr, float bw, float noise);
  void reset();
  void process(float* samples, uint32_t frames);
};

#endif
