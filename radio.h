#ifndef RADIO_H
#define RADIO_H

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string_view>
#include <vector>

inline constexpr float kRadioPi = 3.1415926535f;
inline constexpr float kRadioTwoPi = 6.283185307f;
inline constexpr float kRadioBiquadQ = 0.707f;
inline constexpr float kRadioLinDbFloor = 1e-12f;
inline constexpr float kRadioSoftClipThresholdDefault = 0.98f;
inline constexpr float kRadioHashUnitInv = 1.0f / 4294967295.0f;
inline constexpr size_t kRadioCalibrationBandCount = 12;
inline constexpr size_t kRadioCalibrationFftSize = 1024;
inline constexpr size_t kRadioCalibrationFftBinCount =
    kRadioCalibrationFftSize / 2 + 1;
inline constexpr std::array<float, kRadioCalibrationBandCount>
    kRadioCalibrationBandHz{{125.0f, 250.0f, 400.0f, 630.0f, 1000.0f, 1250.0f,
                             1600.0f, 2000.0f, 2500.0f, 3150.0f, 4000.0f,
                             5000.0f}};

inline const std::array<float, kRadioCalibrationBandCount + 1>&
radioCalibrationBandEdgesHz() {
  static const auto kEdges = [] {
    std::array<float, kRadioCalibrationBandCount + 1> edges{};
    edges[0] = kRadioCalibrationBandHz[0] /
               std::sqrt(kRadioCalibrationBandHz[1] / kRadioCalibrationBandHz[0]);
    for (size_t i = 1; i < kRadioCalibrationBandCount; ++i) {
      edges[i] = std::sqrt(kRadioCalibrationBandHz[i - 1] *
                           kRadioCalibrationBandHz[i]);
    }
    edges[kRadioCalibrationBandCount] =
        kRadioCalibrationBandHz.back() *
        std::sqrt(kRadioCalibrationBandHz.back() /
                  kRadioCalibrationBandHz[kRadioCalibrationBandCount - 2]);
    return edges;
  }();
  return kEdges;
}

inline const std::array<float, kRadioCalibrationFftSize>&
radioCalibrationWindow() {
  static const auto kWindow = [] {
    std::array<float, kRadioCalibrationFftSize> window{};
    for (size_t i = 0; i < kRadioCalibrationFftSize; ++i) {
      window[i] = 0.5f - 0.5f * std::cos(kRadioTwoPi * static_cast<float>(i) /
                                         static_cast<float>(window.size() - 1));
    }
    return window;
  }();
  return kWindow;
}

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
  void setBandpass(float sampleRate, float freq, float q);
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
  float fs = 0.0f;
  float noiseHpHz = 0.0f; // 500.0f
  float noiseLpHz = 0.0f;
  float humHz = 0.0f;
  float humPhase = 0.0f;
  float scEnv = 0.0f;
  float scAtk = 0.0f;
  float scRel = 0.0f;
  float crackleEnv = 0.0f;
  float crackleDecay = 0.0f;
  float pinkFast = 0.0f;
  float pinkSlow = 0.0f;
  float brown = 0.0f;
  float hissDrift = 0.0f;
  float hissDriftSlow = 0.0f;
  float filterQ = 0.0f;
  float scAttackMs = 0.0f;
  float scReleaseMs = 0.0f;
  float crackleDecayMs = 0.0f;
  float sidechainMaskRef = 0.0f;
  float hissMaskDepth = 0.0f;
  float burstMaskDepth = 0.0f;
  float pinkFastPole = 0.0f;
  float pinkSlowPole = 0.0f;
  float brownStep = 0.0f;
  float hissDriftPole = 0.0f;
  float hissDriftNoise = 0.0f;
  float hissDriftSlowPole = 0.0f;
  float hissDriftSlowNoise = 0.0f;
  float whiteMix = 0.0f;
  float pinkFastMix = 0.0f;
  float pinkDifferenceMix = 0.0f;
  float pinkFastSubtract = 0.0f;
  float brownMix = 0.0f;
  float hissBase = 0.0f;
  float hissDriftDepth = 0.0f;
  float hissDriftSlowMix = 0.0f;
  float humSecondHarmonicMix = 0.0f;

  void setFs(float newFs, float noiseBwHz);
  void reset();
  float process(const NoiseInput& in);
};

struct AMDetectorSampleInput {
  float signal = 0.0f;
  float ifNoiseAmp = 0.0f;
};

struct AMDetector {
  float fs = 0.0f;
  int osFactor = 0;
  float bwHz = 0.0f;
  float tuneOffsetHz = 0.0f;

  std::mt19937 rng{0x1942u};
  std::uniform_real_distribution<float> dist{-1.0f, 1.0f};

  Biquad ifHp1;
  Biquad ifHp2;
  Biquad ifLp1;
  Biquad ifLp2;
  Biquad audioHp;
  Biquad audioLp1;

  float audioRect = 0.0f;
  float avcRect = 0.0f;
  float audioEnv = 0.0f;
  float avcEnv = 0.0f;
  float dcEnv = 0.0f;

  float diodeDrop = 0.0f;
  float audioDiodeDrop = 0.0f;
  float avcDiodeDrop = 0.0f;
  float detectorChargeResNorm = 0.0f;
  float detectorDischargeMs = 0.0f;
  float avcChargeMs = 0.0f;
  float avcReleaseMs = 0.0f;

  float detectorChargeCoeff = 0.0f;
  float detectorReleaseCoeff = 0.0f;
  float avcChargeCoeff = 0.0f;
  float avcReleaseCoeff = 0.0f;
  float dcCoeff = 0.0f;

  float detectorGain = 0.0f;
  float controlVoltageRef = 0.0f;

  float dcMs = 0.0f;
  float ifLowBaseHz = 0.0f;
  float ifMinBwHz = 0.0f;
  float ifMaxFraction = 0.0f;
  float audioHpHz = 0.0f;
  float detLpScale = 0.0f;
  float detLpMinHz = 0.0f;

  void init(float newFs, float newBw, float newTuneHz = 0.0f);
  void setBandwidth(float newBw, float newTuneHz = 0.0f);
  void reset();
  float process(const AMDetectorSampleInput& in);
};

struct SpeakerSim {
  Biquad suspensionRes;
  Biquad coneBody;
  Biquad upperBreakup;
  Biquad coneDip;
  Biquad topLp;
  float drive = 0.0f;
  float limit = 0.0f;
  float asymBias = 0.0f;
  float filterQ = 0.0f;
  float suspensionHz = 0.0f;
  float suspensionQ = 0.0f;
  float suspensionGainDb = 0.0f;
  float coneBodyHz = 0.0f;
  float coneBodyQ = 0.0f;
  float coneBodyGainDb = 0.0f;
  float upperBreakupHz = 0.0f;
  float upperBreakupQ = 0.0f;
  float upperBreakupGainDb = 0.0f;
  float coneDipHz = 0.0f;
  float coneDipQ = 0.0f;
  float coneDipGainDb = 0.0f;
  float topLpHz = 0.0f;
  float suspensionComplianceTolerance = 0.0f;
  float coneMassTolerance = 0.0f;
  float breakupTolerance = 0.0f;
  float voiceCoilTolerance = 0.0f;
  float excursionEnv = 0.0f;
  float excursionAtk = 0.0f;
  float excursionRel = 0.0f;
  float excursionRef = 0.0f;
  float complianceLossDepth = 0.0f;
  float hfLossDepth = 0.0f;

  void init(float fs);
  void reset();
  float process(float x, bool& clipped);
};

struct Radio1938;

enum class StageId : uint8_t {
  Tuning,
  Input,
  ControlBus,
  InterferenceDerived,
  FrontEnd,
  Demod,
  ReceiverCircuit,
  Tone,
  Power,
  Noise,
  Speaker,
  Cabinet,
  FinalLimiter,
  OutputClip,
};

struct RadioBlockControl {
  float sampleRate = 48000.0f;
  float tuneNorm = 0.0f;
  float offHz = 0.0f;
};

struct RadioDerivedSampleParams {
  float demodIfNoiseAmp = 0.0f;
  float noiseAmp = 0.0f;
  float crackleAmp = 0.0f;
  float crackleRate = 0.0f;
  float humAmp = 0.0f;
  bool humToneEnabled = true;
};

struct RadioSampleContext {
  const RadioBlockControl* block = nullptr;
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

struct RadioControlBusNode {
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

struct RadioDemodNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float process(Radio1938& radio,
                       float y,
                       const RadioSampleContext& ctx);
};

struct RadioReceiverCircuitNode {
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

struct RadioSpeakerNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float process(Radio1938& radio,
                       float y,
                       const RadioSampleContext& ctx);
};

struct RadioCabinetNode {
  static void init(Radio1938& radio, RadioInitContext& initCtx);
  static void reset(Radio1938& radio);
  static float process(Radio1938& radio,
                       float y,
                       const RadioSampleContext& ctx);
};

struct RadioFinalLimiterNode {
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
    Philco37116X,
  };

  float sampleRate = 0.0f;
  int channels = 1;
  float bwHz = 0.0f;
  float noiseWeight = 0.0f;
  float makeupGain = 0.0f; // baseline unity gain
  float presentationGain = 0.0f;
  Preset preset;
  bool initialized = false;
  Radio1938();

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
    StageId id = StageId::ControlBus;
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
        {StageId::ControlBus, "ControlBus", true, &RadioControlBusNode::update},
        {StageId::InterferenceDerived, "InterferenceDerived", true,
         &RadioInterferenceDerivedNode::update},
    }};

    std::array<ProgramPathStep, 6> programPathSteps{{
        {StageId::Input, "Input", true, &RadioInputNode::process},
        {StageId::FrontEnd, "FrontEnd", true, &RadioFrontEndNode::process},
        {StageId::Demod, "Demod", true, &RadioDemodNode::process},
        {StageId::ReceiverCircuit, "ReceiverCircuit", true,
         &RadioReceiverCircuitNode::process},
        {StageId::Tone, "Tone", true, &RadioToneNode::process},
        {StageId::Power, "Power", true, &RadioPowerNode::process},
    }};

    std::array<PresentationPathStep, 5> presentationPathSteps{{
        {StageId::Noise, "Noise", true, &RadioNoiseNode::process},
        {StageId::Speaker, "Speaker", true, &RadioSpeakerNode::process},
        {StageId::Cabinet, "Cabinet", true, &RadioCabinetNode::process},
        {StageId::FinalLimiter, "FinalLimiter", true,
         &RadioFinalLimiterNode::process},
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
    std::array<ConfigureStep, 10> configureSteps{{
        {StageId::Input, "Input", &RadioInputNode::init},
        {StageId::ControlBus, "ControlBus", &RadioControlBusNode::init},
        {StageId::FrontEnd, "FrontEnd", &RadioFrontEndNode::init},
        {StageId::ReceiverCircuit, "ReceiverCircuit",
         &RadioReceiverCircuitNode::init},
        {StageId::Tone, "Tone", &RadioToneNode::init},
        {StageId::Power, "Power", &RadioPowerNode::init},
        {StageId::Speaker, "Speaker", &RadioSpeakerNode::init},
        {StageId::Cabinet, "Cabinet", &RadioCabinetNode::init},
        {StageId::FinalLimiter, "FinalLimiter", &RadioFinalLimiterNode::init},
        {StageId::OutputClip, "OutputClip", &RadioOutputClipNode::init},
    }};

    std::array<AllocateStep, 0> allocateSteps{};

    std::array<InitializeDependentStateStep, 4> initializeDependentStateSteps{{
        {StageId::Tuning, "Tuning", &RadioTuningNode::init},
        {StageId::Demod, "Demod", &RadioDemodNode::init},
        {StageId::Noise, "Noise", &RadioNoiseNode::init},
        {StageId::InterferenceDerived, "InterferenceDerived",
         &RadioInterferenceDerivedNode::init},
    }};

    std::array<ResetStep, 11> resetSteps{{
        {StageId::ControlBus, "ControlBus", &RadioControlBusNode::reset},
        {StageId::Power, "Power", &RadioPowerNode::reset},
        {StageId::Input, "Input", &RadioInputNode::reset},
        {StageId::FrontEnd, "FrontEnd", &RadioFrontEndNode::reset},
        {StageId::ReceiverCircuit, "ReceiverCircuit",
         &RadioReceiverCircuitNode::reset},
        {StageId::Tone, "Tone", &RadioToneNode::reset},
        {StageId::Demod, "Demod", &RadioDemodNode::reset},
        {StageId::Speaker, "Speaker", &RadioSpeakerNode::reset},
        {StageId::Cabinet, "Cabinet", &RadioCabinetNode::reset},
        {StageId::FinalLimiter, "FinalLimiter", &RadioFinalLimiterNode::reset},
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
    bool finalLimiterActive = false;
    float finalLimiterGain = 1.0f;
    float finalLimiterPeak = 0.0f;
    float finalLimiterDutyCycle = 0.0f;
    float finalLimiterAverageGainReduction = 0.0f;
    float finalLimiterMaxGainReduction = 0.0f;
    float finalLimiterAverageGainReductionDb = 0.0f;
    float finalLimiterMaxGainReductionDb = 0.0f;
    uint32_t finalLimiterActiveSamples = 0;
    uint32_t processedSamples = 0;
    uint32_t powerClipSamples = 0;
    uint32_t speakerClipSamples = 0;
    uint32_t outputClipSamples = 0;
    double finalLimiterGainReductionSum = 0.0;
    double finalLimiterGainReductionDbSum = 0.0;

    void reset() {
      anyClip = false;
      powerClip = false;
      speakerClip = false;
      outputClip = false;
      finalLimiterActive = false;
      finalLimiterGain = 1.0f;
      finalLimiterPeak = 0.0f;
      finalLimiterDutyCycle = 0.0f;
      finalLimiterAverageGainReduction = 0.0f;
      finalLimiterMaxGainReduction = 0.0f;
      finalLimiterAverageGainReductionDb = 0.0f;
      finalLimiterMaxGainReductionDb = 0.0f;
      finalLimiterActiveSamples = 0;
      processedSamples = 0;
      powerClipSamples = 0;
      speakerClipSamples = 0;
      outputClipSamples = 0;
      finalLimiterGainReductionSum = 0.0;
      finalLimiterGainReductionDbSum = 0.0;
    }

    void markPowerClip() {
      anyClip = true;
      powerClip = true;
      powerClipSamples++;
    }

    void markSpeakerClip() {
      anyClip = true;
      speakerClip = true;
      speakerClipSamples++;
    }

    void markOutputClip() {
      anyClip = true;
      outputClip = true;
      outputClipSamples++;
    }
  } diagnostics;

  struct RadioControlSenseState {
    float controlVoltageSense = 0.0f;
    float powerSagSense = 0.0f;

    void reset() {
      controlVoltageSense = 0.0f;
      powerSagSense = 0.0f;
    }
  } controlSense;

  struct RadioControlBusState {
    float controlVoltage = 0.0f;
    float supplySag = 0.0f;
    float controlVoltageAttackMs = 0.0f;
    float controlVoltageReleaseMs = 0.0f;
    float supplySagAttackMs = 0.0f;
    float supplySagReleaseMs = 0.0f;
    float controlVoltageAtk = 0.0f;
    float controlVoltageRel = 0.0f;
    float supplySagAtk = 0.0f;
    float supplySagRel = 0.0f;

    void reset() {
      controlVoltage = 0.0f;
      supplySag = 0.0f;
    }
  } controlBus;

  struct IdentityState {
    uint32_t seed = 0x1937620u;
    float driftDepth = 0.0f;
  } identity;

  struct CalibrationStageMetrics {
    uint64_t sampleCount = 0;
    double rmsIn = 0.0;
    double rmsOut = 0.0;
    float peakIn = 0.0f;
    float peakOut = 0.0f;
    float crestIn = 0.0f;
    float crestOut = 0.0f;
    float spectralCentroidHz = 0.0f;
    float bandwidth3dBHz = 0.0f;
    float bandwidth6dBHz = 0.0f;
    uint64_t clipCountIn = 0;
    uint64_t clipCountOut = 0;
    double inSumSq = 0.0;
    double outSumSq = 0.0;
    std::array<double, kRadioCalibrationBandCount> bandEnergy{};
    std::array<double, kRadioCalibrationFftBinCount> fftBinEnergy{};
    std::array<float, kRadioCalibrationFftSize> fftTimeBuffer{};
    size_t fftFill = 0;
    uint64_t fftBlockCount = 0;

    void clearAccumulators();
    void resetMeasurementState();
    void updateSnapshot(float sampleRate);
  };

  struct CalibrationState {
    static constexpr size_t kStageCount =
        static_cast<size_t>(StageId::OutputClip) + 1u;

    bool enabled = false;
    uint64_t totalSamples = 0;
    uint64_t preLimiterClipCount = 0;
    uint64_t postLimiterClipCount = 0;
    uint64_t limiterActiveSamples = 0;
    float limiterDutyCycle = 0.0f;
    float limiterAverageGainReduction = 0.0f;
    float limiterMaxGainReduction = 0.0f;
    float limiterAverageGainReductionDb = 0.0f;
    float limiterMaxGainReductionDb = 0.0f;
    double limiterGainReductionSum = 0.0;
    double limiterGainReductionDbSum = 0.0;
    std::array<CalibrationStageMetrics, kStageCount> stages{};

    void resetMeasurementState();
    void reset();
  } calibration;

  struct GlobalTuning {
    float oversampleFactor = 0.0f;
    float ifNoiseMix = 0.0f;
    float postNoiseMix = 0.0f;
    float noiseFloorAmp = 0.0f;
    float inputPad = 0.0f;
    bool enableAutoLevel = false;
    float autoTargetDb = 0.0f;
    float autoMaxBoostDb = 0.0f;
    float satClipDelta = 0.0f;
    float satClipMinLevel = 0.0f;
    float outputClipThreshold = 0.0f;
    float oversampleCutoffFraction = 0.0f;
  } globals;

  struct TuningNodeState {
    float tuneOffsetHz = 0.0f;
    float tuneOffsetNorm = 0.0f;
    float tunedBw = 0.0f;
    float tuneAppliedHz = 0.0f;
    float bwAppliedHz = 0.0f;
    float tuneSmoothedHz = 0.0f;
    float bwSmoothedHz = 0.0f;
    float safeBwMinHz = 0.0f;
    float safeBwMaxHz = 0.0f;
    float preBwScale = 0.0f;
    float postBwScale = 0.0f;
    float smoothTau = 0.0f;
    float updateEps = 0.0f;
  } tuning;

  struct InputNodeState {
    float autoEnv = 0.0f;
    float autoGainDb = 0.0f;
    float autoEnvAtk = 0.0f;
    float autoEnvRel = 0.0f;
    float autoGainAtk = 0.0f;
    float autoGainRel = 0.0f;
    float autoEnvAttackMs = 0.0f;
    float autoEnvReleaseMs = 0.0f;
    float autoGainAttackMs = 0.0f;
    float autoGainReleaseMs = 0.0f;
  } input;

  struct FrontEndNodeState {
    float inputHpHz = 0.0f;
    float selectivityPeakHz = 0.0f;
    float selectivityPeakQ = 0.0f;
    float selectivityPeakGainDb = 0.0f;
    Biquad hpf;
    Biquad preLpfIn;
    Biquad preLpfOut;
    Biquad selectivityPeak;
  } frontEnd;

  struct DemodNodeState {
    AMDetector am;
  } demod;

  struct ReceiverCircuitNodeState {
    bool enabled = false;
    float couplingHpHz = 0.0f;
    float couplingCapTolerance = 0.0f;
    float gridLeakTolerance = 0.0f;
    float interstagePeakHz = 0.0f;
    float plateLoadTolerance = 0.0f;
    float interstagePeakQ = 0.0f;
    float interstagePeakGainDb = 0.0f;
    float transformerLpHz = 0.0f;
    float toneCapTolerance = 0.0f;
    float transformerTolerance = 0.0f;
    float presenceDipHz = 0.0f;
    float presenceDipQ = 0.0f;
    float presenceDipGainDb = 0.0f;
    float stageInputRef = 0.0f;
    float driveBase = 0.0f;
    float asymBiasBase = 0.0f;
    float controlVoltageGainDepth = 0.0f;
    float supplySagGainDepth = 0.0f;
    float supplySagDriveDepth = 0.0f;
    float gridConductionStart = 0.0f;
    float gridConductionDepth = 0.0f;
    float stageEnv = 0.0f;
    float stageAtk = 0.0f;
    float stageRel = 0.0f;
    Biquad couplingHp;
    Biquad interstagePeak;
    Biquad presenceDip;
    Biquad transformerLp;
  } receiverCircuit;

  struct ToneNodeState {
    float presenceHz = 0.0f;
    float presenceQ = 0.0f;
    float presenceGainDb = 0.0f;
    float tiltSplitHz = 0.0f;
    Biquad presence;
    Biquad tiltLp;
  } tone;

  struct PowerNodeState {
    float sagEnv = 0.0f;
    float sagAtk = 0.0f;
    float sagRel = 0.0f;
    float sagStart = 0.0f;
    float sagEnd = 0.0f;
    float rectifierPhase = 0.0f;
    float rippleDepth = 0.0f;
    float satOsPrev = 0.0f;
    float satDrive = 0.0f;
    float satMix = 0.0f;
    float sagAttackMs = 0.0f;
    float sagReleaseMs = 0.0f;
    float rectifierMinHz = 0.0f;
    float rippleSecondHarmonicMix = 0.0f;
    float gainSagPerPower = 0.0f;
    float rippleGainBase = 0.0f;
    float rippleGainDepth = 0.0f;
    float gainMin = 0.0f;
    float gainMax = 0.0f;
    float supplyDriveDepth = 0.0f;
    float supplyBiasDepth = 0.0f;
    Biquad postLpf;
    Biquad satOsLpIn;
    Biquad satOsLpOut;
    Saturator sat;
  } power;

  struct NoiseConfig {
    bool enableHumTone = false;
    float humHzDefault = 0.0f;
    float noiseWeightRef = 0.0f;
    float noiseWeightScaleMax = 0.0f;
    float humAmpScale = 0.0f;
    float crackleAmpScale = 0.0f;
    float crackleRateScale = 0.0f;
  } noiseConfig;

  struct NoiseDerivedState {
    float baseNoiseAmp = 0.0f;
    float baseCrackleAmp = 0.0f;
    float baseHumAmp = 0.0f;
    float crackleRate = 0.0f;
  } noiseDerived;

  struct NoiseRuntimeState {
    NoiseHum hum;
  } noiseRuntime;

  struct SpeakerStageState {
    float osPrev = 0.0f;
    float drive = 0.0f;
    Biquad osLpIn;
    Biquad osLpOut;
    SpeakerSim speaker;
  } speakerStage;

  struct CabinetNodeState {
    bool enabled = false;
    float panelHz = 0.0f;
    float panelQ = 0.0f;
    float panelGainDb = 0.0f;
    float chassisHz = 0.0f;
    float chassisQ = 0.0f;
    float chassisGainDb = 0.0f;
    float cavityDipHz = 0.0f;
    float cavityDipQ = 0.0f;
    float cavityDipGainDb = 0.0f;
    float grilleLpHz = 0.0f;
    float rearDelayMs = 0.0f;
    float rearMix = 0.0f;
    float rearHpHz = 0.0f;
    float rearLpHz = 0.0f;
    float panelStiffnessTolerance = 0.0f;
    float baffleLeakTolerance = 0.0f;
    float cavityTolerance = 0.0f;
    float grilleClothTolerance = 0.0f;
    float rearPathTolerance = 0.0f;
    float rearMixApplied = 0.0f;
    int minBufferSamples = 8;
    int bufferGuardSamples = 4;
    std::vector<float> buf;
    int index = 0;
    Biquad panel;
    Biquad chassis;
    Biquad cavityDip;
    Biquad grilleLp;
    Biquad rearHp;
    Biquad rearLp;
  } cabinet;

  struct FinalLimiterNodeState {
    bool enabled = false;
    float threshold = 0.0f;
    float lookaheadMs = 0.0f;
    int delaySamples = 0;
    std::vector<float> delayBuf;
    std::vector<float> requiredGainBuf;
    int delayWriteIndex = 0;
    float attackMs = 0.0f;
    float releaseMs = 0.0f;
    float attackCoeff = 0.0f;
    float gain = 1.0f;
    float targetGain = 1.0f;
    float releaseCoeff = 0.0f;
    float osPrev = 0.0f;
    Biquad osLpIn;
    Biquad osLpOut;
    float observedPeak = 0.0f;
  } finalLimiter;

  struct OutputNodeState {
    float clipOsPrev = 0.0f;
    Biquad clipOsLpIn;
    Biquad clipOsLpOut;
  } output;

  static std::string_view presetName(Preset preset);
  static std::string_view stageName(StageId id);
  bool applyPreset(std::string_view presetName);
  void applyPreset(Preset preset);
  void setIdentitySeed(uint32_t seed);
  void setCalibrationEnabled(bool enabled);
  void resetCalibration();
  void init(int ch, float sr, float bw, float noise);
  void reset();
  void process(float* samples, uint32_t frames);
};

#endif
