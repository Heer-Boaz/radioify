#ifndef RADIO_H
#define RADIO_H

#include "audiofilter/math/biquad.h"
#include "audiofilter/models/resonant_networks.h"
#include "audiofilter/models/transformer_models.h"
#include "audiofilter/models/tube_models.h"
#include "audiofilter/radio1938/models/am_detector.h"
#include "audiofilter/radio1938/models/noise_hum.h"
#include "audiofilter/radio1938/models/speaker_sim.h"
#include "audiofilter/radio1938/radio1938_constants.h"
#include "audiofilter/radio1938/radio_pipeline_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct Radio1938 {
  enum class Preset {
    Philco37116,
    Philco37116X = Philco37116,
  };

  float sampleRate = 0.0f;
  int channels = 1;
  float bwHz = 0.0f;
  float noiseWeight = 0.0f;
  Preset preset = Preset::Philco37116;
  bool initialized = false;
  Radio1938();

  using Graph = RadioGraph;
  using Lifecycle = RadioLifecycle;

  Graph graph = makeRadioGraph();
  Lifecycle lifecycle = makeRadioLifecycle();

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
    float tuningErrorSense = 0.0f;

    void reset() {
      controlVoltageSense = 0.0f;
      powerSagSense = 0.0f;
      tuningErrorSense = 0.0f;
    }
  } controlSense;

  struct RadioControlBusState {
    float controlVoltage = 0.0f;
    float supplySag = 0.0f;

    void reset() {
      controlVoltage = 0.0f;
      supplySag = 0.0f;
    }
  } controlBus;

  struct IdentityState {
    uint32_t seed = 0x1937620u;
    float driftDepth = 0.0f;
    float frontEndAntennaDrift = 1.0f;
    float frontEndRfDrift = 1.0f;
    float mixerDriveDrift = 1.0f;
    float mixerBiasDrift = 1.0f;
    float ifPrimaryDrift = 1.0f;
    float ifSecondaryDrift = 1.0f;
    float ifCouplingDrift = 1.0f;
    float detectorLoadDrift = 1.0f;
  } identity;

  struct CalibrationPassMetrics {
    uint64_t sampleCount = 0;
    double rmsIn = 0.0;
    double rmsOut = 0.0;
    double meanIn = 0.0;
    double meanOut = 0.0;
    float peakIn = 0.0f;
    float peakOut = 0.0f;
    float crestIn = 0.0f;
    float crestOut = 0.0f;
    float spectralCentroidHz = 0.0f;
    float bandwidth3dBHz = 0.0f;
    float bandwidth6dBHz = 0.0f;
    uint64_t clipCountIn = 0;
    uint64_t clipCountOut = 0;
    double inSum = 0.0;
    double outSum = 0.0;
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

  struct CalibrationRmsMetric {
    uint64_t sampleCount = 0;
    double sumSq = 0.0;
    float rms = 0.0f;
    float peak = 0.0f;

    void reset();
    void accumulate(float value);
    void updateSnapshot();
  };

  struct CalibrationState {
    static constexpr size_t kPassCount =
        static_cast<size_t>(PassId::OutputClip) + 1u;

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
    uint64_t validationSampleCount = 0;
    uint64_t driverGridPositiveSamples = 0;
    uint64_t outputGridPositiveSamples = 0;
    uint64_t outputGridAPositiveSamples = 0;
    uint64_t outputGridBPositiveSamples = 0;
    float driverGridPositiveFraction = 0.0f;
    float outputGridPositiveFraction = 0.0f;
    float outputGridAPositiveFraction = 0.0f;
    float outputGridBPositiveFraction = 0.0f;
    float maxMixerPlateCurrentAmps = 0.0f;
    float maxReceiverPlateCurrentAmps = 0.0f;
    float maxDriverPlateCurrentAmps = 0.0f;
    float maxOutputPlateCurrentAAmps = 0.0f;
    float maxOutputPlateCurrentBAmps = 0.0f;
    double interstageSecondarySumSq = 0.0;
    float interstageSecondaryRmsVolts = 0.0f;
    float interstageSecondaryPeakVolts = 0.0f;
    float maxSpeakerSecondaryVolts = 0.0f;
    float maxSpeakerReferenceRatio = 0.0f;
    float maxDigitalOutput = 0.0f;
    uint64_t detectorIfCrackleEventCount = 0;
    float detectorIfCrackleMaxBurstAmp = 0.0f;
    float detectorIfCrackleMaxEnv = 0.0f;
    CalibrationRmsMetric detectorNodeVolts;
    CalibrationRmsMetric receiverGridVolts;
    CalibrationRmsMetric receiverPlateSwingVolts;
    CalibrationRmsMetric driverGridVolts;
    CalibrationRmsMetric driverPlateSwingVolts;
    CalibrationRmsMetric outputGridAVolts;
    CalibrationRmsMetric outputGridBVolts;
    CalibrationRmsMetric outputPrimaryVolts;
    CalibrationRmsMetric speakerSecondaryVolts;
    bool validationDriverGridPositive = false;
    bool validationFailed = false;
    bool validationOutputGridPositive = false;
    bool validationOutputGridAPositive = false;
    bool validationOutputGridBPositive = false;
    bool validationSpeakerOverReference = false;
    bool validationInterstageSecondary = false;
    bool validationDcShift = false;
    bool validationDigitalClip = false;
    std::array<CalibrationPassMetrics, kPassCount> passes{};

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
    float outputClipThreshold = 0.0f;
    float oversampleCutoffFraction = 0.0f;
  } globals;

  struct IqInputState {
    float iqPhase = 0.0f;

    void resetRuntime() { iqPhase = 0.0f; }
  } iqInput;

  struct TuningNodeState {
    bool magneticTuningEnabled = false;
    float tuneOffsetHz = 0.0f;
    float afcCorrectionHz = 0.0f;
    float afcCaptureHz = 0.0f;
    float afcMaxCorrectionHz = 0.0f;
    float afcDeadband = 0.0f;
    float afcResponseMs = 0.0f;
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
    float sourceCarrierHz = 0.0f;
    uint32_t configRevision = 0;
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
    float sourceResistanceOhms = 0.0f;
    float inputResistanceOhms = 1.0f;
    float couplingCapFarads = 0.0f;
    float sourceDivider = 1.0f;
    Biquad sourceCouplingHp;
  } input;

  struct FrontEndNodeState {
    uint32_t appliedConfigRevision = 0;
    float inputHpHz = 0.0f;
    float rfGain = 0.0f;
    float avcGainDepth = 0.0f;
    float selectivityPeakHz = 0.0f;
    float selectivityPeakQ = 0.0f;
    float selectivityPeakGainDb = 0.0f;
    float antennaInductanceHenries = 0.0f;
    float antennaCapacitanceFarads = 0.0f;
    float antennaSeriesResistanceOhms = 0.0f;
    float antennaLoadResistanceOhms = 0.0f;
    float rfInductanceHenries = 0.0f;
    float rfCapacitanceFarads = 0.0f;
    float rfSeriesResistanceOhms = 0.0f;
    float rfLoadResistanceOhms = 0.0f;
    Biquad hpf;
    Biquad preLpfIn;
    Biquad preLpfOut;
    Biquad selectivityPeak;
    SeriesRlcBandpass antennaTank;
    SeriesRlcBandpass rfTank;
  } frontEnd;

  struct MixerNodeState {
    float rfGridDriveVolts = 0.0f;
    float loGridDriveVolts = 0.0f;
    float loGridBiasVolts = 0.0f;
    float avcGridDriveVolts = 0.0f;
    float plateSupplyVolts = 0.0f;
    float plateDcVolts = 0.0f;
    float plateQuiescentVolts = 0.0f;
    float screenVolts = 0.0f;
    float biasVolts = 0.0f;
    float cutoffVolts = 0.0f;
    float modelCutoffVolts = 0.0f;
    float plateCurrentAmps = 0.0f;
    float plateQuiescentCurrentAmps = 0.0f;
    float mutualConductanceSiemens = 0.0f;
    float acLoadResistanceOhms = 0.0f;
    float plateKneeVolts = 0.0f;
    float gridSoftnessVolts = 0.0f;
    float plateResistanceOhms = 0.0f;
    float operatingPointToleranceVolts = 35.0f;
    float mixedBaseGridVolts = 0.0f;
    float conversionGain = 1.0f;
    float inputDriveEnv = 0.0f;
  } mixer;

  struct IFStripNodeState {
    bool enabled = false;
    uint32_t appliedConfigRevision = 0;
    float ifMinBwHz = 0.0f;
    float stageGain = 0.0f;
    float avcGainDepth = 0.0f;
    float ifCenterHz = 0.0f;
    float primaryInductanceHenries = 0.0f;
    float secondaryInductanceHenries = 0.0f;
    float secondaryLoadResistanceOhms = 0.0f;
    float interstageCouplingCoeff = 0.0f;
    float outputCouplingCoeff = 0.0f;
    float sourceCarrierHz = 0.0f;
    float loFrequencyHz = 0.0f;
    // Phase used only to strip the real RF source carrier down to a complex
    // source envelope. This is not the IF tuning phase.
    float sourceDownmixPhase = 0.0f;
    // Phase used only for the residual tuning offset between the source
    // envelope and the IF tuned center. Do not fold the full LO phase into
    // this baseband state or the detector feed will collapse.
    float ifEnvelopePhase = 0.0f;
    SourceInputMode prevSourceMode = SourceInputMode::ComplexEnvelope;
    float senseLowHz = 0.0f;
    float senseHighHz = 0.0f;
    float demodBandwidthHz = 0.0f;
    float demodTuneOffsetHz = 0.0f;
    // Active IF runtime: a tuned-can equivalent mapped onto source downmix
    // plus one loaded complex envelope transfer feeding the detector.
    IQBiquad sourceEnvelope;
    IQBiquad loadedCanEnvelope;
  } ifStrip;

  struct DemodNodeState {
    AMDetector am;
    uint32_t appliedConfigRevision = 0;
  } demod;

  struct DetectorAudioNodeState {
    uint32_t appliedConfigRevision = 0;
    // Reduced-order second-detector audio branch fed from the demodulated
    // detector output. This is separate from the slower AVC storage path inside
    // AMDetector; do not turn it into an extra synthetic bandwidth knob.
    float audioNode = 0.0f;
    float audioEnv = 0.0f;
    bool warmStartPending = true;
    Biquad postLp;
  } detectorAudio;

  struct ReceiverCircuitNodeState {
    bool enabled = false;
    float volumeControlResistanceOhms = 0.0f;
    float volumeControlTapResistanceOhms = 0.0f;
    float volumeControlPosition = 1.0f;
    float volumeControlLoudnessResistanceOhms = 0.0f;
    float volumeControlLoudnessCapFarads = 0.0f;
    float volumeControlTapVoltage = 0.0f;
    float couplingCapFarads = 0.0f;
    float gridLeakResistanceOhms = 0.0f;
    // Reduced-order load seen by the detector-audio node, derived from the
    // explicit pot / loudness / first-audio grid path. This is not a generic
    // gain knob.
    float detectorLoadConductance = 0.0f;
    // Dynamic state of the receiver input network driven from the detector:
    // coupling-cap voltage, first-audio grid AC voltage, and loudness-tap
    // capacitor state. These belong to the analog-equivalent receiver input,
    // not to the detector solve itself.
    float couplingCapVoltage = 0.0f;
    float gridVoltage = 0.0f;
    bool warmStartPending = true;
    float tubePlateSupplyVolts = 0.0f;
    float tubePlateDcVolts = 0.0f;
    float tubeQuiescentPlateVolts = 0.0f;
    float tubeScreenVolts = 0.0f;
    float tubeBiasVolts = 0.0f;
    float tubePlateCurrentAmps = 0.0f;
    float tubeQuiescentPlateCurrentAmps = 0.0f;
    float tubeMutualConductanceSiemens = 0.0f;
    float tubeMu = 0.0f;
    bool tubeTriodeConnected = false;
    float tubeLoadResistanceOhms = 0.0f;
    float tubePlateKneeVolts = 0.0f;
    float tubeGridSoftnessVolts = 0.0f;
    KorenTriodeModel tubeTriodeModel{};
    KorenTriodeLut tubeTriodeLut;
    float tubePlateResistanceOhms = 0.0f;
    float operatingPointToleranceVolts = 35.0f;
    float tubePlateVoltage = 0.0f;
    float osPrevGridVolts = 0.0f;
    Biquad osLpIn;
    Biquad osLpOut;
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
    struct CenterTappedInterstageState {
      float primaryCurrent = 0.0f;
      float primaryVoltage = 0.0f;
      float secondaryACurrent = 0.0f;
      float secondaryAVoltage = 0.0f;
      float secondaryBCurrent = 0.0f;
      float secondaryBVoltage = 0.0f;
    };

    float sagEnv = 0.0f;
    float sagAtk = 0.0f;
    float sagRel = 0.0f;
    float sagStart = 0.0f;
    float sagEnd = 0.0f;
    float rectifierPhase = 0.0f;
    float rippleDepth = 0.0f;
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
    float postLpHz = 0.0f;
    float gridCouplingCapFarads = 0.0f;
    float gridLeakResistanceOhms = 0.0f;
    float gridCouplingCapVoltage = 0.0f;
    float gridVoltage = 0.0f;
    float driverSourceResistanceOhms = 0.0f;
    float tubePlateSupplyVolts = 0.0f;
    float tubePlateDcVolts = 0.0f;
    float tubeQuiescentPlateVolts = 0.0f;
    float tubeScreenVolts = 0.0f;
    float tubeBiasVolts = 0.0f;
    float tubePlateCurrentAmps = 0.0f;
    float tubeQuiescentPlateCurrentAmps = 0.0f;
    float tubeMutualConductanceSiemens = 0.0f;
    float tubeMu = 0.0f;
    bool tubeTriodeConnected = false;
    float tubeAcLoadResistanceOhms = 0.0f;
    float tubePlateKneeVolts = 0.0f;
    float tubeGridSoftnessVolts = 0.0f;
    float tubeGridCurrentResistanceOhms = 0.0f;
    KorenTriodeModel tubeTriodeModel{};
    KorenTriodeLut tubeTriodeLut;
    float tubePlateResistanceOhms = 0.0f;
    float operatingPointToleranceVolts = 35.0f;
    float tubePlateVoltage = 0.0f;
    float interstagePrimaryLeakageInductanceHenries = 0.0f;
    float interstageMagnetizingInductanceHenries = 0.0f;
    float interstageTurnsRatioPrimaryToSecondary = 0.0f;
    float interstagePrimaryResistanceOhms = 0.0f;
    float interstagePrimaryCoreLossResistanceOhms = 0.0f;
    float interstagePrimaryShuntCapFarads = 0.0f;
    float interstageSecondaryLeakageInductanceHenries = 0.0f;
    float interstageSecondaryResistanceOhms = 0.0f;
    float interstageSecondaryShuntCapFarads = 0.0f;
    int interstageIntegrationSubsteps = 1;
    float outputGridLeakResistanceOhms = 0.0f;
    float outputGridCurrentResistanceOhms = 0.0f;
    float outputTubePlateSupplyVolts = 0.0f;
    float outputTubePlateDcVolts = 0.0f;
    float outputTubeQuiescentPlateVolts = 0.0f;
    float outputTubeBiasVolts = 0.0f;
    float outputTubePlateCurrentAmps = 0.0f;
    float outputTubeQuiescentPlateCurrentAmps = 0.0f;
    float outputTubeMutualConductanceSiemens = 0.0f;
    float outputTubeMu = 0.0f;
    float outputTubePlateToPlateLoadOhms = 0.0f;
    float outputTubePlateKneeVolts = 0.0f;
    float outputTubeGridSoftnessVolts = 0.0f;
    KorenTriodeModel outputTubeTriodeModel{};
    KorenTriodeLut outputTubeTriodeLut;
    float outputTubePlateResistanceOhms = 0.0f;
    float outputGridAVolts = 0.0f;
    float outputGridBVolts = 0.0f;
    float outputOperatingPointToleranceVolts = 35.0f;
    float outputTransformerPrimaryLeakageInductanceHenries = 0.0f;
    float outputTransformerMagnetizingInductanceHenries = 0.0f;
    float outputTransformerTurnsRatioPrimaryToSecondary = 0.0f;
    float outputTransformerPrimaryResistanceOhms = 0.0f;
    float outputTransformerPrimaryCoreLossResistanceOhms = 0.0f;
    float outputTransformerPrimaryShuntCapFarads = 0.0f;
    float outputTransformerSecondaryLeakageInductanceHenries = 0.0f;
    float outputTransformerSecondaryResistanceOhms = 0.0f;
    float outputTransformerSecondaryShuntCapFarads = 0.0f;
    int outputTransformerIntegrationSubsteps = 1;
    float outputLoadResistanceOhms = 0.0f;
    float nominalOutputPowerWatts = 0.0f;
    float internalSampleRate = 0.0f;
    float osPrevInput = 0.0f;
    CenterTappedInterstageState interstageCt;
    CurrentDrivenTransformer interstageTransformer;
    CurrentDrivenTransformer outputTransformer;
    bool outputTransformerAffineReady = false;
    std::array<float, 16> outputTransformerAffineStateA{};
    CurrentDrivenTransformerSample outputTransformerAffineSlope;
    Biquad osLpIn;
    Biquad osLpOut;
    Biquad postLpf;
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
    Biquad clarifier1;
    Biquad clarifier2;
    Biquad clarifier3;
    float clarifier1Hz = 0.0f;
    float clarifier1Q = 0.0f;
    float clarifier1Coupling = 0.0f;
    float clarifier2Hz = 0.0f;
    float clarifier2Q = 0.0f;
    float clarifier2Coupling = 0.0f;
    float clarifier3Hz = 0.0f;
    float clarifier3Q = 0.0f;
    float clarifier3Coupling = 0.0f;
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
    // Derived clean speaker-secondary peak from the modeled output stage.
    float digitalReferenceSpeakerVoltsPeak = 1.0f;
    float digitalMakeupGain = 1.0f;
    float clipOsPrev = 0.0f;
    Biquad clipOsLpIn;
    Biquad clipOsLpOut;
  } output;

  static std::string_view presetName(Preset preset);
  static std::string_view passName(PassId id);
  bool applyPreset(std::string_view presetName);
  void applyPreset(Preset preset);
  void setIdentitySeed(uint32_t seed);
  void setCalibrationEnabled(bool enabled);
  void resetCalibration();
  float resolvedInputCarrierHz() const;
  void warmInputCarrier(float receivedCarrierRmsVolts,
                        uint32_t warmupFrames = 16384u);
  void resetAfterCarrierWarmup();
  void init(int ch, float sr, float bw, float noise);
  void reset();
  void processIfReal(float* samples, uint32_t frames);
  void processAmAudio(const float* audioSamples,
                      float* outSamples,
                      uint32_t frames,
                      float receivedCarrierRmsVolts,
                      float modulationIndex);
  void processIqBaseband(const float* iqInterleaved,
                         float* outSamples,
                         uint32_t frames);
};

bool applyRadioSettingsIni(Radio1938& radio,
                          const std::string& path,
                          const std::string& presetName = std::string(),
                          std::string* error = nullptr);

#endif
