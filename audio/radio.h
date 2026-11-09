#ifndef RADIO_H
#define RADIO_H

#include "audiofilter/math/biquad.h"
#include "audiofilter/math/oversampling.h"
#include "audiofilter/math/iq_fir_lowpass.h"
#include "audiofilter/models/resonant_networks.h"
#include "audiofilter/models/rectifier_supply.h"
#include "audiofilter/models/coupled_if_transformer.h"
#include "audiofilter/models/transformer_models.h"
#include "audiofilter/models/tube_models.h"
#include "audiofilter/radio1938/models/am_detector.h"
#include "audiofilter/radio1938/models/noise_hum.h"
#include "audiofilter/radio1938/models/speaker_sim.h"
#include "audiofilter/radio1938/models/radio_room.h"
#include "audiofilter/radio1938/radio1938_constants.h"
#include "audiofilter/radio1938/radio_pipeline_types.h"
#include "audiofilter/radio1938/radio_receiver_profile.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

struct RadioAmReceptionSample;

struct Radio1938 {
  float sampleRate = 0.0f;
  int channels = 1;
  float bwHz = 0.0f;
  float noiseWeight = 0.0f;
  RadioReceiverProfile receiverProfile = kDefaultRadioReceiverProfile;
  bool initialized = false;
  RadioRoomConfig room;
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
    float tuningSignalVolts = 0.0f;

    void reset() {
      controlVoltageSense = 0.0f;
      powerSagSense = 0.0f;
      tuningErrorSense = 0.0f;
      tuningSignalVolts = 0.0f;
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
    float speakerReferenceRmsRatio = 0.0f;
    float maxDigitalOutput = 0.0f;
    float detectorWaveformAverageSolveSteps = 0.0f;
    float detectorWaveformSplitFraction = 0.0f;
    float detectorStorageAverageIterations = 0.0f;
    uint32_t detectorStorageMaxIterations = 0;
    double detectorAfcProbeMs = 0.0;
    double detectorIslandMs = 0.0;
    double detectorStorageSolveMs = 0.0;
    float detectorAfcProbeUsPerSample = 0.0f;
    float detectorIslandUsPerSample = 0.0f;
    float detectorStorageSolveUsPerCall = 0.0f;
    float powerOutputAverageNewtonIterations = 0.0f;
    uint32_t powerOutputMaxNewtonIterations = 0;
    float powerOutputAverageTransformerSubsteps = 0.0f;
    float powerInterstageAverageIterations = 0.0f;
    uint32_t powerInterstageMaxIterations = 0;
    float powerInterstageAverageSubsteps = 0.0f;
    double powerInterstageSolveMs = 0.0;
    double powerOutputSolveMs = 0.0;
    float powerInterstageSolveUsPerSample = 0.0f;
    float powerOutputSolveUsPerSample = 0.0f;
    float powerInterstageDriverEvalCallsPerSample = 0.0f;
    uint64_t powerInterstageAdaptiveValidationAttempts = 0;
    uint64_t powerInterstageAdaptiveAcceptedDownshifts = 0;
    float powerInterstageAdaptiveAcceptedDownshiftFraction = 0.0f;
    float powerInterstageAdaptiveAverageBoundaryErrorVolts = 0.0f;
    float powerInterstageAdaptiveMaxBoundaryErrorVolts = 0.0f;
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
    std::vector<CalibrationPassMetrics> passes =
        std::vector<CalibrationPassMetrics>(kPassCount);

    struct ClickTraceEvent {
      uint64_t sampleIndex = 0;
      float previousOutput = 0.0f;
      float output = 0.0f;
      float delta = 0.0f;
      std::array<float, kPassCount> previousPassOutput{};
      std::array<float, kPassCount> passOutput{};
      std::array<float, kPassCount> passDelta{};
      std::array<bool, kPassCount> passValid{};
    };

    struct ClickTraceState {
      bool enabled = false;
      float threshold = 0.05f;
      size_t maxEvents = 12;
      uint64_t sampleIndex = 0;
      bool hasPreviousSample = false;
      float previousOutput = 0.0f;
      std::array<float, kPassCount> previousPassOutput{};
      std::array<float, kPassCount> currentPassOutput{};
      std::array<bool, kPassCount> previousPassValid{};
      std::array<bool, kPassCount> currentPassValid{};
      std::vector<ClickTraceEvent> events;

      void resetRuntime();
    } clickTrace;

    void resetMeasurementState();
    void reset();
  } calibration;

  struct GlobalTuning {
    float oversampleFactor = 0.0f;  // Exactly 1, 2, 4 or 8; presets use 2.
    float ifNoiseMix = 0.0f;
    float postNoiseMix = 0.0f;
    float noiseFloorAmp = 0.0f;
    float inputPad = 0.0f;
    bool enableAutoLevel = false;
    float autoTargetDb = 0.0f;
    float autoMaxBoostDb = 0.0f;
    float outputClipThreshold = 0.0f;
    // Flat passband edge / host rate. Halfband stop begins at 1 - this value.
    float oversampleCutoffFraction = 0.0f;
  } globals;

  struct IqInputState {
    float carrierOscCos = 1.0f;
    float carrierOscSin = 0.0f;
    std::vector<float> rfScratch;
    // Transmitted envelope history for a delayed propagation path.
    static constexpr uint32_t kEnvelopeHistory = 512;
    std::array<float, kEnvelopeHistory> envelopeHistory{};
    uint32_t envelopeWrite = 0;

    void pushEnvelope(float envelope) {
      envelopeWrite = (envelopeWrite + 1) & (kEnvelopeHistory - 1);
      envelopeHistory[envelopeWrite] = envelope;
    }
    // Four-point Lagrange interpolation, delaySamples in [0, kEnvelopeHistory - 4].
    float delayedEnvelope(float delaySamples) const {
      const int whole = std::max(1, static_cast<int>(delaySamples));
      const float mu = delaySamples - static_cast<float>(whole);
      const auto at = [&](int age) {
        return envelopeHistory[(envelopeWrite - static_cast<uint32_t>(age)) &
                               (kEnvelopeHistory - 1)];
      };
      const float cm1 = -mu * (mu - 1) * (mu - 2) / 6;
      const float c0 = (mu + 1) * (mu - 1) * (mu - 2) / 2;
      const float c1 = -(mu + 1) * mu * (mu - 2) / 2;
      const float c2 = (mu + 1) * mu * (mu - 1) / 6;
      return cm1 * at(whole - 1) + c0 * at(whole) + c1 * at(whole + 1) + c2 * at(whole + 2);
    }

    void resetRuntime() {
      carrierOscCos = 1.0f;
      carrierOscSin = 0.0f;
      envelopeHistory.fill(0.0f);
      envelopeWrite = 0;
    }
  } iqInput;

  struct TuningNodeState {
    bool magneticTuningEnabled = false;
    float tuneOffsetHz = 0.0f;
    float afcCorrectionHz = 0.0f;
    float afcCaptureHz = 0.0f;
    float afcMaxCorrectionHz = 0.0f;
    // Discriminator/control gain is independent of the reactance-tube pull limit.
    float afcLoopGain = 8.0f;
    float afcDeadband = 0.0f;
    float afcResponseMs = 0.0f;
    float afcSignalReferenceVolts = 0.05f;
    float afcTick = 0.0f;
    float tunedBw = 0.0f;
    // Oscillator offset, including AFC pull.
    float tuneAppliedHz = 0.0f;
    // Mechanical dial offset only; the ganged RF circuits follow it.
    float dialAppliedHz = 0.0f;
    float bwAppliedHz = 0.0f;
    float tuneSmoothedHz = 0.0f;
    float dialSmoothedHz = 0.0f;
    float bwSmoothedHz = 0.0f;
    float safeBwMinHz = 0.0f;
    float safeBwMaxHz = 0.0f;
    // Broadcast-band dial position; sets the RF circuits' loaded bandwidth.
    float stationFrequencyHz = 1000000.0f;
    float smoothTau = 0.0f;
    float updateEps = 0.0f;
    float sourceCarrierHz = 0.0f;
    float sourceBandwidthHz = 0.0f;
    uint32_t updateCountdown = 0;
    float smoothingTick = 0;
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
    bool secondTunedCircuitEnabled = true;
    uint32_t appliedConfigRevision = 0;
    // Grid volts of the first tube per volt of standard-antenna EMF at
    // resonance: the step-up of the antenna coupling coil.
    float antennaVoltageGain = 0.0f;
    // Remote-cutoff RF amplifier between the two tuned circuits; present only
    // with the second circuit. Its gain is gm, which follows the full AVC
    // line, times the transfer impedance of its RF transformer at resonance.
    float rfStageLoadOhms = 0.0f;
    float rfStageBiasVolts = -3.0f;
    float rfStageScreenVolts = 100.0f;
    float appliedAvcVolts = -1.0f;
    float appliedScreenScale = -1.0f;
    float avcGain = 1.0f;
    // Ganged circuits aligned to the dial. Shunt loads are the reflected
    // antenna, RF-plate and grid damping, not physical resistors.
    float antennaInductanceHenries = 0.0f;
    float antennaCoilQ = 0.0f;
    float antennaShuntLoadOhms = 0.0f;
    float rfInductanceHenries = 0.0f;
    float rfCoilQ = 0.0f;
    float rfShuntLoadOhms = 0.0f;
    float antennaLoadedQ = 0.0f;
    float rfLoadedQ = 0.0f;
    // Frequency the gang is set to; it excludes the AFC oscillator pull.
    double tunedHz = 0.0;
    TunedRfCircuit antennaTank;
    TunedRfCircuit rfTank;
    // Representation conversion of the real virtual-carrier RF to the source
    // envelope. Image rejection is independent of receiver selectivity.
    IQFirLowpass sourceEnvelope;
    float sourceDownmixStepCos = 1.0f;
    float sourceDownmixStepSin = 0.0f;
    float sourceDownmixOscCos = 1.0f;
    float sourceDownmixOscSin = 0.0f;
    // Rotation from the source frame to the mechanical dial frame.
    float dialStepCos = 1.0f;
    float dialStepSin = 0.0f;
    float dialOscCos = 1.0f;
    float dialOscSin = 0.0f;
  } frontEnd;

  struct MixerNodeState {
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
    float plateKneeVolts = 0.0f;
    float gridSoftnessVolts = 0.0f;
    float plateResistanceOhms = 0.0f;
    float operatingPointToleranceVolts = 35.0f;
    FixedPlatePentodeEvaluator plateCurrentForGrid;
    float effectiveLoGridBiasVolts = 0.0f;
    // Specimen tolerance of the conversion transconductance.
    float conversionDrift = 1.0f;
    float inputDriveEnvCoeff = 0.0f;
    float mixedBaseGridVolts = 0.0f;
    // Conversion transconductance in siemens: IF plate current per signal-
    // grid volt, at the present AVC bias.
    float conversionGain = 1.0f;
    float inputDriveEnv = 0.0f;
  } mixer;

  struct IFStripNodeState {
    bool enabled = false;
    uint32_t appliedConfigRevision = 0;
    float ifMinBwHz = 0.0f;
    // The converter's plate current drives the first transformer; each later
    // transformer is driven by a remote-cutoff IF tube whose gm follows the
    // AVC line (one tube per transformer after the first).
    int avcStageCount = 1;
    float stageBiasVolts = -3.0f;
    float stageScreenVolts = 100.0f;
    float appliedAvcVolts = -1.0f;
    float appliedScreenScale = -1.0f;
    float avcGain = 1.0f;
    float ifCenterHz = 0.0f;
    float primaryInductanceHenries = 0.0f;
    float secondaryInductanceHenries = 0.0f;
    float secondaryLoadResistanceOhms = 0.0f;
    float primarySeriesResistanceOhms = 0.0f;
    float secondarySeriesResistanceOhms = 0.0f;
    // gm of the IF tubes at their operating point without AVC.
    float stageTransconductanceSiemens = 0.0f;
    // Peak plate swing an IF tube can deliver into its can (supply less the
    // pentode knee); a lightning crash saturates there. Zero disables it,
    // for linear checks.
    float stagePlateSwingLimitVolts = 0.0f;
    // gm(AVC) / gm(no AVC) of one IF tube.
    float stageAvcRatio = 1.0f;
    int transformerCount = 2;
    float interstageCouplingCoeff = 0.0f;
    float outputCouplingCoeff = 0.0f;
    float sourceCarrierHz = 0.0f;
    float loFrequencyHz = 0.0f;
    // Phase used only for the residual tuning offset between the source
    // envelope and the IF tuned center. Do not fold the full LO phase into
    // this baseband state or the detector feed will collapse.
    float ifEnvelopePhase = 0.0f;
    float ifEnvelopeStepRadians = 0.0f;
    float ifEnvelopeStepCos = 1.0f;
    float ifEnvelopeStepSin = 0.0f;
    float ifEnvelopeOscCos = 1.0f;
    float ifEnvelopeOscSin = 0.0f;
    SourceInputMode prevSourceMode = SourceInputMode::ComplexEnvelope;
    float senseLowHz = 0.0f;
    float senseHighHz = 0.0f;
    float demodBandwidthHz = 0.0f;
    float demodTuneOffsetHz = 0.0f;
    std::array<CoupledIfTransformer, 3> transformers;
    std::array<float, 2> previousIfInput{};
    std::mt19937 noiseRng{0x1942u};
    std::uniform_real_distribution<float> noiseDist{-1.0f, 1.0f};
    float noiseSampleScale = 1.0f;
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
    // DC driving-point conductance, for diagnostics and operating-point probes.
    // Playback uses the complete affine RC companion (including capacitor
    // history current), not this conductance alone.
    float detectorLoadConductance = 0.0f;
    // Dynamic state of the receiver input network driven from the detector:
    // coupling-cap voltage, first-audio grid AC voltage, and loudness-tap
    // capacitor state. These belong to the analog-equivalent receiver input,
    // not to the detector solve itself.
    float couplingCapVoltage = 0.0f;
    float couplingCapCurrent = 0.0f;
    float volumeControlTapCapCurrent = 0.0f;
    // Voltage on the loudness capacitor (series R-C from tap to ground).
    float volumeControlTapCapVoltage = 0.0f;
    float gridVoltage = 0.0f;
    bool warmStartPending = true;
    float tubePlateSupplyVolts = 0.0f;
    float tubePlateDcVolts = 0.0f;
    float tubeQuiescentPlateVolts = 0.0f;
    float tubeBiasVolts = 0.0f;
    float tubeQuiescentPlateCurrentAmps = 0.0f;
    float tubeMu = 0.0f;
    bool tubeTriodeConnected = false;
    float tubeLoadResistanceOhms = 0.0f;
    float tubeDcFeedResistanceOhms = 0.0f;
    // Bypass capacitor from the first-audio plate to ground (0 = none).
    float tubePlateShuntCapFarads = 0.0f;
    float tubeSupplyVolts = 0;
    double tubeSupplyCurrentAmps = 0;
    float tubeEffectiveSupplyVolts = 0.0f;
    KorenTriodeModel tubeTriodeModel{};
    KorenTriodeLut tubeTriodeLut;
    float tubePlateResistanceOhms = 0.0f;
    float operatingPointToleranceVolts = 35.0f;
    float tubePlateVoltage = 0.0f;
    Oversampler oversampler;
  } receiverCircuit;

  struct ToneNodeState {
    float presenceHz = 0.0f;
    float presenceQ = 0.0f;
    float presenceGainDb = 0.0f;
    float tiltSplitHz = 0.0f;
    float tiltDepthDb = 0.0f;
    Biquad presence;
    Biquad tiltLp;
  } tone;

  struct PowerSupplyState {
    RectifierSupply::Config config;
    RectifierSupply circuit;
    float otherLoadAmps = 0;
    float auxiliaryOtherLoadAmps = 0;
    float screenCurrentRatio = 0;
    // Idle main B+ the supply is started at; the RF and IF screen dividers
    // hang on the main node, through an RC branch where the set has one,
    // and follow it in proportion.
    float mainReferenceVolts = 0;
    // Up to two cascaded RC sections from the main node; R = 0 is a wire.
    std::array<float, 2> screenFilterResistanceOhms{};
    std::array<float, 2> screenFilterFarads{};
    std::array<float, 2> screenFilterCoeff{1, 1};
    std::array<float, 2> screenFilterVolts{};
    float screenSupplyVolts = 0;
  } powerSupply;

  struct PowerNodeState {
    enum class Topology : uint8_t {
      TransformerCoupledPushPull,
      CapCoupledSingleEnded,
    };

    struct CenterTappedInterstageState {
      float primaryCurrent = 0.0f;
      float primaryVoltage = 0.0f;
      float secondaryACurrent = 0.0f;
      float secondaryAVoltage = 0.0f;
      float secondaryBCurrent = 0.0f;
      float secondaryBVoltage = 0.0f;
    };

    Topology topology = Topology::TransformerCoupledPushPull;
    float postLpHz = 0.0f;
    float gridCouplingCapFarads = 0.0f;
    float gridLeakResistanceOhms = 0.0f;
    float gridCouplingCapVoltage = 0.0f;
    float gridVoltage = 0.0f;
    float driverSourceResistanceOhms = 0.0f;
    // AC voltage on the first-audio plate bypass capacitor.
    float driverPlateCapVoltage = 0.0f;
    // Optional potentiometer from the coupling node to a capacitor to ground,
    // with the grid on its wiper (Philco 37-116: 106, 500 k, and 107, 110 pF).
    // Position is the wiper height above the capacitor end: 1 puts the grid
    // on the coupling node. 0 ohms disables the network.
    float gridPotentiometerOhms = 0.0f;
    float gridPotentiometerPosition = 1.0f;
    float gridPotentiometerCapFarads = 0.0f;
    float gridPotentiometerCapVoltage = 0.0f;
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
    float outputTubeScreenVolts = 0.0f;
    float outputTubeBiasVolts = 0.0f;
    float outputTubeCutoffVolts = 0.0f;
    float outputTubePlateCurrentAmps = 0.0f;
    float outputTubeQuiescentPlateCurrentAmps = 0.0f;
    float outputTubeMutualConductanceSiemens = 0.0f;
    float outputTubeMu = 0.0f;
    float outputTubePlateToPlateLoadOhms = 0.0f;
    float outputTubePlateKneeVolts = 0.0f;
    float outputTubeGridSoftnessVolts = 0.0f;
    KorenTriodeModel outputTubeTriodeModel{};
    KorenTriodeLut outputTubeTriodeLut;
    FixedPlatePentodeEvaluator outputTubePentodeCurrentForGrid;
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
    CenterTappedInterstageState interstageCt;
    CurrentDrivenTransformer interstageTransformer;
    CurrentDrivenTransformer outputTransformer;
    uint64_t outputTransformerSubstepCount = 0;
    uint64_t outputNewtonIterationCount = 0;
    uint32_t outputNewtonMaxIterations = 0;
    uint64_t interstageSubstepCount = 0;
    uint64_t interstageIterationCount = 0;
    uint32_t interstageMaxIterations = 0;
    uint64_t interstageSolveTimeNs = 0;
    uint64_t interstageDriverEvalCount = 0;
    uint64_t interstageAdaptiveValidationAttemptCount = 0;
    uint64_t interstageAdaptiveAcceptedDownshiftCount = 0;
    double interstageAdaptiveBoundaryErrorSumVolts = 0.0;
    float interstageAdaptiveBoundaryErrorMaxVolts = 0.0f;
    uint64_t outputSolveTimeNs = 0;
    int interstageAdaptiveSubsteps = 0;
    int interstageAdaptiveValidationCountdown = 0;
    Oversampler oversampler;
    // Measurement output shares the internal sample clock and needs its own
    // decimator history; taking the final substep alone aliases terminal data.
    Oversampler terminalResampler;
    Biquad postLpf;
  } power;

  struct NoiseConfig {
    bool enableHumTone = false;
    float humHzDefault = 0.0f;
    float humAmpScale = 0.0f;
  } noiseConfig;

  struct NoiseDerivedState {
    float baseNoiseAmp = 0.0f;
    float baseHumAmp = 0.0f;
  } noiseDerived;

  struct NoiseRuntimeState {
    NoiseHum hum;
    // Residual texture is defined in the old force-equivalent domain. Convert
    // only that layer to pressure; receiver noise already traverses the motor.
    Biquad radiation;
  } noiseRuntime;

  struct SpeakerStageState {
    float drive = 0.0f;
    float physicalDriveVolts = 0.0f;
    Oversampler oversampler;
    SpeakerSim speaker;
  } speakerStage;

  struct CabinetNodeState {
    bool enabled = false;
    // Empirical total mounting loss; not a geometry-derived dipole corner.
    float mountingHpHz = 0.0f;
    float mountingHpQ = 0.0f;
    Biquad mountingHp;
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
    float clarifier1AttenuationDb = 0.0f;
    float clarifier2Hz = 0.0f;
    float clarifier2Q = 0.0f;
    float clarifier2AttenuationDb = 0.0f;
    float clarifier3Hz = 0.0f;
    float clarifier3Q = 0.0f;
    float clarifier3AttenuationDb = 0.0f;
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
    Oversampler peakOversampler;
    float observedPeak = 0.0f;
  } finalLimiter;

  struct OutputNodeState {
    // Derived clean speaker-secondary peak from the modeled output stage.
    float digitalReferenceSpeakerVoltsPeak = 1.0f;
    float digitalMakeupGain = 1.0f;
    Oversampler clipOversampler;
  } output;

  static std::string_view passName(PassId id);
  bool applyReceiverProfile(std::string_view profileName);
  void applyReceiverProfile(RadioReceiverProfile profile);
  void setIdentitySeed(uint32_t seed);
  void setCalibrationEnabled(bool enabled);
  void resetCalibration();
  float resolvedInputCarrierHz() const;
  // Settle the receiver on an unmodulated carrier arriving with the given
  // complex path gain, so programme starts without a turn-on transient.
  void warmInputCarrier(float receivedCarrierRmsVolts,
                        uint32_t warmupFrames = 16384u,
                        float pathGainI = 1.0f,
                        float pathGainQ = 0.0f);
  void clearWarmupObservations();
  void init(int ch, float sr, float bw, float noise);
  void reset();
  void processIfReal(float* samples, uint32_t frames);
  // Optional observation: frames mono terminal-voltage samples in a separate,
  // caller-owned buffer. The complete audio graph runs unchanged.
  void processAmAudio(const float* audioSamples,
                      float* outSamples,
                      uint32_t frames,
                      float receivedCarrierRmsVolts,
                      float modulationIndex,
                      const RadioAmReceptionSample* receptionSamples = nullptr,
                      float* terminalVolts = nullptr);
  void processIqBaseband(const float* iqInterleaved,
                         float* outSamples,
                         uint32_t frames);
};

bool applyRadioSettingsIni(Radio1938& radio,
                          const std::string& path,
                          const std::string& presetName = std::string(),
                          std::string* error = nullptr);

#endif
