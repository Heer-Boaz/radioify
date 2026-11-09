#include "radio.h"
#include "receiver_input_reference.h"
#include "audiofilter/radio1938/audio_pipeline_execution.h"
#include "audiofilter/radio1938/radio_am_ingress.h"
#include "audiofilter/radio1938/preview/radio_preview_pipeline.h"
#include "audiofilter/math/signal_math.h"
#include "core/runtime_defaults.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr float kDefaultSampleRate = 48000.0f;
constexpr float kDefaultBandwidthHz = 5000.0f;
// The playback default: the everyday-1938 field on the IRE standard antenna.
const float kDefaultCarrierRmsVolts = radioReceptionAntennaCarrierRmsVolts(
    radioReceptionConfigForProfile(RadioReceptionProfile::Everyday1938));
constexpr float kDefaultModulationIndex = 0.85f;
constexpr float kDefaultNoiseWeight = 0.0f;
constexpr float kDefaultNoiseOnlyWeight = 0.012f;
constexpr uint32_t kWarmupFrames = 16384u;
constexpr uint32_t kSteadyTestFrames = 24000u;
constexpr uint32_t kEnvelopeTestFrames = 9600u;
constexpr float kSweepProgramPeak = 0.35f;
constexpr float kNominalProgramPeak = 1.0f;
constexpr float kImdTonePeak = 0.25f;
constexpr float kOverdriveProgramPeak = 0.95f;
constexpr size_t kSpectrumSize = 4096u;
constexpr double kAmSidebandRatioMin = 0.90;
constexpr double kAmSidebandRatioMax = 1.10;
constexpr double kAmModIndexTolerance = 0.10;
constexpr std::array<float, 20> kSweepFrequenciesHz = {
    50.0f,   75.0f,   100.0f,  150.0f,  200.0f,
    300.0f,  400.0f,  600.0f,  800.0f,  1000.0f,
    1250.0f, 1600.0f, 2000.0f, 2500.0f, 3150.0f,
    4000.0f, 4500.0f, 5000.0f, 5500.0f, 6000.0f};
constexpr std::array<float, 11> kPowerBenchInputPeaks = {
    0.25f, 0.50f, 0.75f, 1.00f, 1.25f, 1.50f,
    2.00f, 2.50f, 3.00f, 3.50f, 4.00f};

struct HarnessConfig {
  float sampleRate = kDefaultSampleRate;
  float bandwidthHz = kDefaultBandwidthHz;
  float carrierRmsVolts = kDefaultCarrierRmsVolts;
  float modulationIndex = kDefaultModulationIndex;
  float noiseWeight = kDefaultNoiseWeight;
  float noiseOnlyWeight = kDefaultNoiseOnlyWeight;
  float detectorWaveformSamplesPerCycle = 0.0f;
  int detectorWaveformMaxSubsteps = 0;
  std::string settingsPath;
  std::string presetName;
  RadioReceiverProfile receiverProfile = kDefaultRadioReceiverProfile;
  bool receiverProfileExplicit = false;
  std::string section = "all";
  bool observeTerminalVoltage = false;
  // Negative keeps the receiver's own listening setting.
  float volumeOverride = -1.0f;
  float toneHz = 1000.0f;
  float crashAreaVoltSeconds = 1e-6f;
  RadioReceptionProfile reception = RadioReceptionProfile::Everyday1938;
  bool atmosphericNoise = false;
  float traceDetailMs = -1.0f;
  float traceSeconds = 10.0f;
  // Without the loudness branch, keep the 1 kHz level of the listening
  // setting rather than the knob position.
  bool levelMatchedFlat = false;
  float speakerNaturalHzOverride = 0.0f;
  // Removes the volume-control loudness branch, leaving the receiver's own
  // bandwidth; the branch is then checked against an impedance reference.
  bool withoutLoudnessBranch = false;
};

// --- Target ranges for harness validation (historical/model anchors) ---
struct ReceiverReferenceBands {
  float terminalLow3dBMin;
  float terminalLow3dBMax;
  float terminalHigh3dBMin;
  float terminalHigh3dBMax;
  float terminalImdDbMax;
  float terminalSinadDbMin;
  float acousticPeakHigh3dBMin;
  float acousticPeakHigh3dBMax;
  float acousticImdDbMax;
  float acousticSinadDbMin;
  float speakerReferenceRatioMin;
  float speakerReferenceRatioMax;
  float nominalOutputPowerMinWatts;
  float nominalOutputPowerMaxWatts;
};

// Response/quality bounds are engineering guards, not measured Philco data.
// Terminal-voltage observations have no acoustic low-pass suppressing noise
// and harmonics; the former acoustically weighted SINAD cannot be reused here.
// See docs/radio1938-period-acoustics.md for both measurements and their limits.
constexpr ReceiverReferenceBands kPhilco37116ReferenceBands{
    // IMD at the living-room setting with the schematic's 0.22 s AVC bus:
    // -46.3 dB at the terminals and -39.1 dB acoustic.
    60.0f, 200.0f, 2500.0f, 5500.0f, -38.0f, 30.0f,
    // Living-room level: 0.18 W on the reference tone, the same electrical
    // power as the typical set (0.11 of the reference volts, below the
    // loudness tap). Rated 12..18 W capacity and the headroom guards stay.
    2500.0f, 5500.0f, -35.0f, 28.0f, 0.09f, 0.14f, 12.0f, 18.0f};

// The mass-market profile deliberately validates against its own 2 W,
// single-ended receiver class. Reusing the 37-116's 15 W / high-fidelity
// bands would turn the test into a demand that both physical models converge.
constexpr ReceiverReferenceBands kTypical1930sReferenceBands{
    100.0f, 250.0f, 1800.0f, 4500.0f, -23.0f, 18.0f,
    2200.0f, 3500.0f, -18.0f, 15.0f, 0.18f, 0.40f, 1.5f, 2.5f};

constexpr const ReceiverReferenceBands& referenceBandsForProfile(
    RadioReceiverProfile profile) {
  switch (profile) {
    case RadioReceiverProfile::Philco37116:
      return kPhilco37116ReferenceBands;
    case RadioReceiverProfile::Typical1930s:
      return kTypical1930sReferenceBands;
  }
  return kPhilco37116ReferenceBands;
}

// Dominant release poles of the actual 110/110 pF and 100/100 pF ladders,
// rather than the retired common 350 pF / 160 k surrogate.
constexpr float kPhilcoDetectorTauUsMin = 30, kPhilcoDetectorTauUsMax = 45;
constexpr float kTypicalDetectorTauUsMin = 90, kTypicalDetectorTauUsMax = 120;
// The 38-12's documented 2 M / 0.03 uF AVC network is 60 ms; leave room for
// its single-precision product.
constexpr float kTypicalAvcTauMsMin = 55.0f, kTypicalAvcTauMsMax = 90.0f;
// The 37-116's bus charges through 92 (1 M) into 90, 90A and the four
// .05 uF grid-return bypasses, 0.22 uF in all (Bulletin 258).
constexpr float kPhilcoAvcTauMsMin = 200.0f, kPhilcoAvcTauMsMax = 240.0f;
// Second harmonic of an 80 Hz tone at 50% modulation, speaker terminals.
// Philco: the schematic's 0.22 s AVC bus leaves -31.5 dBc (-54.6 dBc with
// ten times the capacitance); the former 75 ms prior gave -21 dBc. 38-12:
// -18 dBc from its single-ended output stage and small output transformer,
// within 1 dB with ten times its AVC capacitance; regression bound only.
constexpr double kPhilcoTerminal80HzH2DbcMax = -28.0;
constexpr double kTypicalTerminal80HzH2DbcMax = -15.0;
// The playback limiter holds true peaks below its 0.97 threshold.
constexpr float kTargetMaxDigitalOutputMin = 0.0f;
constexpr float kTargetMaxDigitalOutputMax = 0.975f;
// IF center and nominal output power
constexpr float kTargetIfCenterMinHz = 469000.0f;
constexpr float kTargetIfCenterMaxHz = 471000.0f;
// Clip/flag targets: expect zero in nominal bench
constexpr int kTargetFlagZero = 0;


struct SignalStats {
  double mean = 0.0;
  double rms = 0.0;
  double peak = 0.0;
  double posPeak = 0.0;
  double negPeak = 0.0;
};

struct RunResult {
  Radio1938 radio;
  std::vector<float> output;
};

struct EnvelopeTraceResult {
  Radio1938 radio;
  std::vector<float> output;
  std::vector<float> detectorNode;
  std::vector<float> audioEnv;
  std::vector<float> avcEnv;
};

struct EdgeMetrics {
  double lowLevel = 0.0;
  double highLevel = 0.0;
  double rise10To90Ms = 0.0;
  double fall90To10Ms = 0.0;
  double overshoot = 0.0;
  double undershoot = 0.0;
  double dcShift = 0.0;
};

struct ToneMetrics {
  double dc = 0.0;
  double totalRms = 0.0;
  double signalRms = 0.0;
  double residualRms = 0.0;
  double sinadDb = -300.0;
  double sndrDb = -300.0;
};

struct SweepPoint {
  float frequencyHz = 0.0f;
  double inputRms = 0.0;
  double outputRms = 0.0;
  double gainDb = -300.0;
  double dcShift = 0.0;
};

struct SweepSummary {
  double peakGainDb = -300.0;
  double low3dBHz = 0.0;
  double high3dBHz = 0.0;
};

struct AmValidationSummary {
  double maxOutputAbsDiff = 0.0;
  double carrierRms = 0.0;
  double sideUpperRms = 0.0;
  double sideLowerRms = 0.0;
  double expectedSideRms = 0.0;
  double sidebandRatio = 0.0;
  bool sidebandRatioInBand = false;
  double detectorDc = 0.0;
  double detectorTone = 0.0;
  double detectorToneToDcRatio = 0.0;
  double modulationIndexConfig = 0.0;
  double effectiveModulationDepth = 0.0;
  double modulationIndexMeasuredEnv = 0.0;
  bool modulationIndexInBand = false;
  double detectorSinadDb = -300.0;
  double outputSinadDb = -300.0;
  double speakerReferenceRatio = 0.0;
  double maxDigitalOutput = 0.0;
  double ifCenterHz = 0.0;
  double nominalOutputPowerWatts = 0.0;
  int waveformSubsteps = 0;
};

struct ReferenceAnchorSummary {
  double sinadNominalDb = -300.0;
  double speakerReferenceRatio = 0.0;
  double maxDigitalOutput = 0.0;
  int waveformSubsteps = 0;
};

struct PowerOnlyTraceResult {
  Radio1938 radio;
  std::vector<float> output;
  std::vector<float> speakerCurrent;
  std::vector<float> outputGridA;
  std::vector<float> outputGridB;
};

struct PowerBenchPoint {
  float inputPeak = 0.0f;
  double secondaryRms = 0.0;
  double secondaryPeak = 0.0;
  double secondaryCurrentRms = 0.0;
  double loadRealOhms = 0.0;
  double loadImagOhms = 0.0;
  double loadMagOhms = 0.0;
  double reflectedLoadRealOhms = 0.0;
  double reflectedLoadMagOhms = 0.0;
  double sndrDb = -300.0;
  double thdDb = -300.0;
  double gridAPositiveFraction = 0.0;
  double gridBPositiveFraction = 0.0;
  int powerClip = 0;
};

[[noreturn]] void fail(const std::string& message) {
  throw std::runtime_error(message);
}

double dbFromRatio(double ratio) {
  return (ratio > 1e-20) ? 20.0 * std::log10(ratio) : -300.0;
}

double effectiveDetectorAudioTauSeconds(const Radio1938& radio) {
  const auto& d = radio.demod.am;
  const double dischargeG = d.audioDischargeResistanceOhms > 0
      ? 1.0 / d.audioDischargeResistanceOhms : 0;
  const double receiverLoadG = std::max<double>(radio.receiverCircuit.detectorLoadConductance, 0);
  const double loadR = 1 / std::max(dischargeG + receiverLoadG, 1e-12);
  // Dominant release pole of the two-capacitor RC ladder with the diode off.
  const double a = d.filterResistanceOhms * loadR * d.detectorStorageCapFarads * d.filterCapFarads;
  const double b = d.filterResistanceOhms * d.detectorStorageCapFarads +
      loadR * (d.detectorStorageCapFarads + d.filterCapFarads);
  return .5 * (b + std::sqrt(std::max(b * b - 4 * a, 0.0)));
}

double avcTauSeconds(const Radio1938& radio) {
  return std::max<double>(radio.demod.am.avcFilterResistanceOhms, 1e-6) *
         std::max<double>(radio.demod.am.avcFilterCapFarads, 1e-12);
}

float amProgramNormScale(const std::vector<float>& program) {
  float maxAbs = 0.0f;
  for (float sample : program) {
    maxAbs = std::max(maxAbs, std::fabs(sample));
  }
  if (maxAbs > 1.0f) {
    return 1.0f / maxAbs;
  }
  return 1.0f;
}

float nominalHarnessModulationIndex(const HarnessConfig& config) {
  return std::clamp(config.modulationIndex, 0.0f, 0.95f);
}

float parseFloatArg(char** argv, int& index, int argc, const char* name) {
  if (index + 1 >= argc) {
    fail(std::string("missing value for ") + name);
  }
  ++index;
  return std::strtof(argv[index], nullptr);
}

int parseIntArg(char** argv, int& index, int argc, const char* name) {
  if (index + 1 >= argc) {
    fail(std::string("missing value for ") + name);
  }
  ++index;
  return std::strtol(argv[index], nullptr, 10);
}

void printUsage() {
  std::cout
      << "radio_measurements [options]\n"
      << "  --sample-rate <hz>\n"
      << "  --bw <hz>\n"
      << "  --carrier-rms <volts>\n"
      << "  --mod-index <value>\n"
      << "  --terminal  Speaker-terminal volts instead of acoustic output\n"
      << "  --without-loudness  Remove the tapped-control loudness branch\n"
      << "  --reception <profile>  Reception profile for preview_trace\n"
      << "  --crash-area <volt-seconds>  Antenna impulse area for the crash section\n"
      << "  --atmospheric  Enable atmospheric noise in preview_trace\n"
      << "  --trace-seconds <s>  Length of preview_trace (default 10)\n"
      << "  --tone-hz <hz>  Test-tone frequency for the overdrive section (default 1000)\n"
      << "  --noise <value>\n"
      << "  --noise-only <value>\n"
      << "  --detector-spc <value>\n"
      << "  --detector-max-substeps <count>\n"
      << "  --speaker-natural-hz <hz>  Override the unmeasured mechanical prior for comparisons\n"
      << "  --radio-settings <path>\n"
      << "  --receiver-profile <typical-1930s|philco-37-116>  Base receiver before settings\n"
      << "  --preset <name>  TOML preset; a built-in name also selects the receiver unless explicitly overridden\n"
      << "  --section <all|config|sweep|envelope|levels|imd|overdrive|sinad|weak|avc|sensitivity|crash|preview_trace|fading|transient|reference|noise|spectrum|am_validate|am_convergence|power_bench|programme>\n";
}

HarnessConfig parseArgs(int argc, char** argv) {
  HarnessConfig config;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--sample-rate") {
      config.sampleRate = parseFloatArg(argv, i, argc, "--sample-rate");
    } else if (arg == "--bw") {
      config.bandwidthHz = parseFloatArg(argv, i, argc, "--bw");
    } else if (arg == "--carrier-rms") {
      config.carrierRmsVolts = parseFloatArg(argv, i, argc, "--carrier-rms");
    } else if (arg == "--reception") {
      if (i + 1 >= argc || !parseRadioReceptionProfile(argv[++i], config.reception))
        fail("unknown reception profile");
    } else if (arg == "--trace-detail-ms") {
      config.traceDetailMs = parseFloatArg(argv, i, argc, "--trace-detail-ms");
    } else if (arg == "--atmospheric") {
      config.atmosphericNoise = true;
    } else if (arg == "--trace-seconds") {
      config.traceSeconds = parseFloatArg(argv, i, argc, "--trace-seconds");
    } else if (arg == "--crash-area") {
      config.crashAreaVoltSeconds = parseFloatArg(argv, i, argc, "--crash-area");
    } else if (arg == "--terminal") {
      config.observeTerminalVoltage = true;
    } else if (arg == "--without-loudness") {
      config.withoutLoudnessBranch = true;
      config.levelMatchedFlat = true;
    } else if (arg == "--mod-index") {
      config.modulationIndex = parseFloatArg(argv, i, argc, "--mod-index");
    } else if (arg == "--noise") {
      config.noiseWeight = parseFloatArg(argv, i, argc, "--noise");
    } else if (arg == "--noise-only") {
      config.noiseOnlyWeight = parseFloatArg(argv, i, argc, "--noise-only");
    } else if (arg == "--detector-spc") {
      config.detectorWaveformSamplesPerCycle =
          parseFloatArg(argv, i, argc, "--detector-spc");
    } else if (arg == "--detector-max-substeps") {
      config.detectorWaveformMaxSubsteps =
          parseIntArg(argv, i, argc, "--detector-max-substeps");
    } else if (arg == "--tone-hz") {
      config.toneHz = parseFloatArg(argv, i, argc, "--tone-hz");
    } else if (arg == "--speaker-natural-hz") {
      config.speakerNaturalHzOverride =
          parseFloatArg(argv, i, argc, "--speaker-natural-hz");
      if (!std::isfinite(config.speakerNaturalHzOverride) ||
          config.speakerNaturalHzOverride <= 0.0f) {
        fail("invalid speaker natural-frequency override");
      }
    } else if (arg == "--radio-settings") {
      if (i + 1 >= argc) fail("missing value for --radio-settings");
      config.settingsPath = argv[++i];
    } else if (arg == "--preset") {
      if (i + 1 >= argc) fail("missing value for --preset");
      config.presetName = argv[++i];
    } else if (arg == "--receiver-profile") {
      if (i + 1 >= argc) fail("missing value for --receiver-profile");
      const std::string_view name = argv[++i];
      if (!parseRadioReceiverProfile(name, config.receiverProfile))
        fail("unknown receiver profile: " + std::string(name));
      config.receiverProfileExplicit = true;
    } else if (arg == "--section") {
      if (i + 1 >= argc) fail("missing value for --section");
      config.section = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      printUsage();
      std::exit(0);
    } else {
      fail(std::string("unknown argument: ") + std::string(arg));
    }
  }
  if (!config.presetName.empty()) {
    RadioReceiverProfile namedProfile{};
    const bool builtIn = parseRadioReceiverProfile(config.presetName, namedProfile);
    if (config.settingsPath.empty()) {
      if (!builtIn) fail("unknown receiver profile: " + config.presetName);
      if (config.receiverProfileExplicit && config.receiverProfile != namedProfile)
        fail("conflicting --preset and --receiver-profile without a settings file");
    }
    if (builtIn && !config.receiverProfileExplicit) config.receiverProfile = namedProfile;
  }
  if (config.speakerNaturalHzOverride > 0.0f &&
      config.speakerNaturalHzOverride >= 0.45f * config.sampleRate) {
    fail("speaker natural-frequency override exceeds the sample-rate limit");
  }
  return config;
}

void warmCarrier(Radio1938& radio, float carrierRmsVolts) {
  radio.warmInputCarrier(carrierRmsVolts, kWarmupFrames);
}

void applyHarnessDetectorWaveformSettings(const HarnessConfig& config,
                                          Radio1938& radio) {
  if (config.detectorWaveformSamplesPerCycle > 0.0f) {
    radio.demod.am.waveformSamplesPerCycle =
        config.detectorWaveformSamplesPerCycle;
  }
  if (config.detectorWaveformMaxSubsteps > 0) {
    radio.demod.am.waveformMaxSubsteps = config.detectorWaveformMaxSubsteps;
  }
}

Radio1938 makeRadio(const HarnessConfig& config, float noiseWeight,
                    bool warmWithCarrier) {
  Radio1938 radio;
  // Receiver identity and TOML overlay selection are separate. A settings
  // file must never silently replace a requested Philco with the default set.
  radio.applyReceiverProfile(config.receiverProfile);
  radio.init(1, config.sampleRate, config.bandwidthHz, noiseWeight);
  if (!config.settingsPath.empty()) {
    std::string error;
    if (!applyRadioSettingsIni(radio, config.settingsPath, config.presetName,
                               &error)) {
      fail("failed to apply radio settings: " + error);
    }
  }
  applyHarnessDetectorWaveformSettings(config, radio);
  if (config.volumeOverride >= 0.0f)
    radio.receiverCircuit.volumeControlPosition = config.volumeOverride;
  if (config.withoutLoudnessBranch) {
    auto& receiver = radio.receiverCircuit;
    if (config.levelMatchedFlat && receiver.volumeControlTapResistanceOhms > 0.0f) {
      // Below the tap the branch takes most of the mid-band signal, so the
      // same knob position without it would overdrive the audio chain. Turn
      // the plain control to the same 1 kHz grid level instead.
      const std::complex<double> laplace{0, 2 * 3.14159265358979323846 * 1000};
      const double target = std::abs(receiverInputReference(receiver, laplace).grid);
      auto plain = receiver;
      double low = 0, high = 1;
      for (int step = 0; step < 40; ++step) {
        plain.volumeControlPosition = static_cast<float>(.5 * (low + high));
        (std::abs(receiverInputReference(plain, laplace, false).grid) < target ? low
                                                                                 : high) =
            plain.volumeControlPosition;
      }
      receiver.volumeControlPosition = static_cast<float>(.5 * (low + high));
    }
    receiver.volumeControlLoudnessResistanceOhms = 0.0f;
    receiver.volumeControlLoudnessCapFarads = 0.0f;
  }
  if (config.speakerNaturalHzOverride > 0.0f) {
    radio.speakerStage.speaker.suspensionHz = config.speakerNaturalHzOverride;
    radio.init(1, config.sampleRate, config.bandwidthHz, noiseWeight);
  }
  radio.setCalibrationEnabled(true);
  if (warmWithCarrier) {
    warmCarrier(radio, config.carrierRmsVolts);
  } else {
    radio.resetCalibration();
    radio.diagnostics.reset();
  }
  return radio;
}

RunResult runAmProgram(const HarnessConfig& config,
                       const std::vector<float>& program,
                       float modulationIndex,
                       float noiseWeight) {
  RunResult result{makeRadio(config, noiseWeight, true), {}};
  result.output.resize(program.size(), 0.0f);
  std::vector<float> terminal;
  if (config.observeTerminalVoltage) terminal.resize(program.size());
  result.radio.processAmAudio(program.data(), result.output.data(),
                              static_cast<uint32_t>(program.size()),
                              config.carrierRmsVolts, modulationIndex, nullptr,
                              terminal.empty() ? nullptr : terminal.data());
  if (config.observeTerminalVoltage) result.output = std::move(terminal);
  return result;
}

RunResult runRfInput(const HarnessConfig& config,
                     const std::vector<float>& rfSamples,
                     float noiseWeight) {
  RunResult result{makeRadio(config, noiseWeight, false), rfSamples};
  result.radio.processIfReal(result.output.data(),
                             static_cast<uint32_t>(result.output.size()));
  return result;
}

void configurePowerOnlyGraph(Radio1938& radio) {
  constexpr std::array<PassId, 20> kDisabledPasses = {
      PassId::Tuning,
      PassId::Input,
      PassId::AVC,
      PassId::AFC,
      PassId::ControlBus,
      PassId::InterferenceDerived,
      PassId::FrontEnd,
      PassId::Mixer,
      PassId::IFStrip,
      PassId::Demod,
      PassId::DetectorAudio,
      PassId::ReceiverInputNetwork,
      PassId::ReceiverCircuit,
      PassId::Tone,
      PassId::Noise,
      PassId::Speaker,
      PassId::Cabinet,
      PassId::OutputScale,
      PassId::FinalLimiter,
      PassId::OutputClip,
  };
  for (PassId id : kDisabledPasses) {
    radio.graph.setEnabled(id, false);
  }
  radio.graph.setEnabled(PassId::Power, true);
}

PowerOnlyTraceResult runPowerOnlyProgram(const HarnessConfig& config,
                                         const std::vector<float>& input) {
  PowerOnlyTraceResult result{makeRadio(config, 0.0f, false), {}, {}, {}, {}};
  configurePowerOnlyGraph(result.radio);
  result.radio.resetCalibration();
  result.radio.diagnostics.reset();
  result.output.resize(input.size(), 0.0f);
  result.speakerCurrent.resize(input.size(), 0.0f);
  result.outputGridA.resize(input.size(), 0.0f);
  result.outputGridB.resize(input.size(), 0.0f);

  RadioBlockControl block{};
  beginRadioPipelineBlock(result.radio, static_cast<uint32_t>(input.size()), block);
  for (size_t i = 0; i < input.size(); ++i) {
    RadioSampleContext ctx{};
    ctx.block = &block;
    runRadioPipelineSample(result.radio, input[i], ctx, PassId::Power);
    result.output[i] = result.radio.power.outputTransformer.secondaryVoltage;
    result.speakerCurrent[i] = result.radio.speakerStage.speaker.electricalCurrentAmps;
    result.outputGridA[i] = result.radio.power.outputGridAVolts;
    result.outputGridB[i] = result.radio.power.outputGridBVolts;
  }
  finishRadioPipelineBlock(result.radio);
  return result;
}

EnvelopeTraceResult runAmProgramWithEnvelopeTrace(const HarnessConfig& config,
                                                  const std::vector<float>& program,
                                                  float modulationIndex,
                                                  float noiseWeight) {
  EnvelopeTraceResult result{makeRadio(config, noiseWeight, true), {}, {}, {}, {}};
  result.output.resize(program.size(), 0.0f);
  result.detectorNode.resize(program.size(), 0.0f);
  result.audioEnv.resize(program.size(), 0.0f);
  result.avcEnv.resize(program.size(), 0.0f);

  for (size_t i = 0; i < program.size(); ++i) {
    result.radio.processAmAudio(&program[i], &result.output[i], 1u,
                                config.carrierRmsVolts, modulationIndex);
    result.detectorNode[i] = result.radio.demod.am.detectorNode;
    result.audioEnv[i] = result.radio.detectorAudio.audioEnv;
    result.avcEnv[i] = result.radio.demod.am.avcEnv;
  }
  return result;
}

EnvelopeTraceResult runRfInputWithTrace(const HarnessConfig& config,
                                        const std::vector<float>& rfSamples,
                                        float noiseWeight) {
  EnvelopeTraceResult result{makeRadio(config, noiseWeight, false), {}, {}, {}, {}};
  result.output.resize(rfSamples.size(), 0.0f);
  result.detectorNode.resize(rfSamples.size(), 0.0f);
  result.audioEnv.resize(rfSamples.size(), 0.0f);
  result.avcEnv.resize(rfSamples.size(), 0.0f);

  std::array<float, 1> sample{};
  for (size_t i = 0; i < rfSamples.size(); ++i) {
    sample[0] = rfSamples[i];
    result.radio.processIfReal(sample.data(), 1u);
    result.output[i] = sample[0];
    result.detectorNode[i] = result.radio.demod.am.detectorNode;
    result.audioEnv[i] = result.radio.detectorAudio.audioEnv;
    result.avcEnv[i] = result.radio.demod.am.avcEnv;
  }
  return result;
}

SignalStats measureSignalStats(const std::vector<float>& samples,
                               size_t startIndex = 0u) {
  SignalStats stats;
  if (startIndex >= samples.size()) return stats;
  double sum = 0.0;
  double sumSq = 0.0;
  double posPeak = -std::numeric_limits<double>::infinity();
  double negPeak = std::numeric_limits<double>::infinity();
  for (size_t i = startIndex; i < samples.size(); ++i) {
    double x = samples[i];
    sum += x;
    sumSq += x * x;
    posPeak = std::max(posPeak, x);
    negPeak = std::min(negPeak, x);
  }
  double count = static_cast<double>(samples.size() - startIndex);
  if (count <= 0.0) return stats;
  stats.mean = sum / count;
  stats.rms = std::sqrt(sumSq / count);
  stats.posPeak = std::isfinite(posPeak) ? posPeak : 0.0;
  stats.negPeak = std::isfinite(negPeak) ? negPeak : 0.0;
  stats.peak = std::max(std::fabs(stats.posPeak), std::fabs(stats.negPeak));
  return stats;
}

double measureToneRms(const std::vector<float>& samples,
                      float sampleRate,
                      float frequencyHz,
                      size_t startIndex) {
  if (startIndex >= samples.size() || sampleRate <= 0.0f || frequencyHz <= 0.0f) {
    return 0.0;
  }
  double iSum = 0.0;
  double qSum = 0.0;
  size_t count = samples.size() - startIndex;
  for (size_t n = 0; n < count; ++n) {
    double phase = kRadioTwoPi * static_cast<double>(frequencyHz) *
                   static_cast<double>(n) / static_cast<double>(sampleRate);
    double x = samples[startIndex + n];
    iSum += x * std::cos(phase);
    qSum -= x * std::sin(phase);
  }
  double peakAmp = 2.0 * std::sqrt(iSum * iSum + qSum * qSum) /
                   std::max<double>(static_cast<double>(count), 1.0);
  return peakAmp / std::sqrt(2.0);
}

std::complex<double> measureTonePhasor(const std::vector<float>& samples,
                                       float sampleRate,
                                       float frequencyHz,
                                       size_t startIndex) {
  if (startIndex >= samples.size() || sampleRate <= 0.0f || frequencyHz <= 0.0f) {
    return {};
  }
  const size_t count = samples.size() - startIndex;
  if (count == 0) return {};
  double mean = 0.0;
  for (size_t n = 0; n < count; ++n) {
    mean += samples[startIndex + n];
  }
  mean /= static_cast<double>(count);

  std::complex<double> sum(0.0, 0.0);
  for (size_t n = 0; n < count; ++n) {
    double phase = kRadioTwoPi * static_cast<double>(frequencyHz) *
                   static_cast<double>(n) / static_cast<double>(sampleRate);
    std::complex<double> rot(std::cos(phase), -std::sin(phase));
    sum += (static_cast<double>(samples[startIndex + n]) - mean) * rot;
  }
  return (2.0 / std::max<double>(static_cast<double>(count), 1.0)) * sum;
}

ToneMetrics measureToneMetrics(const std::vector<float>& samples,
                               float sampleRate,
                               float frequencyHz,
                               size_t startIndex) {
  ToneMetrics metrics;
  if (startIndex >= samples.size() || sampleRate <= 0.0f || frequencyHz <= 0.0f) {
    return metrics;
  }
  const size_t count = samples.size() - startIndex;
  if (count == 0) return metrics;

  double sum = 0.0;
  for (size_t n = 0; n < count; ++n) {
    sum += samples[startIndex + n];
  }
  metrics.dc = sum / static_cast<double>(count);

  double cSum = 0.0;
  double sSum = 0.0;
  double totalSumSq = 0.0;
  double residualSumSq = 0.0;
  for (size_t n = 0; n < count; ++n) {
    double phase = kRadioTwoPi * static_cast<double>(frequencyHz) *
                   static_cast<double>(n) / static_cast<double>(sampleRate);
    double centered = samples[startIndex + n] - metrics.dc;
    cSum += centered * std::cos(phase);
    sSum += centered * std::sin(phase);
    totalSumSq += samples[startIndex + n] * samples[startIndex + n];
  }

  double cosPeak = 2.0 * cSum / static_cast<double>(count);
  double sinPeak = 2.0 * sSum / static_cast<double>(count);
  metrics.signalRms =
      std::sqrt(cosPeak * cosPeak + sinPeak * sinPeak) / std::sqrt(2.0);
  metrics.totalRms = std::sqrt(totalSumSq / static_cast<double>(count));

  for (size_t n = 0; n < count; ++n) {
    double phase = kRadioTwoPi * static_cast<double>(frequencyHz) *
                   static_cast<double>(n) / static_cast<double>(sampleRate);
    double fitted =
        metrics.dc + cosPeak * std::cos(phase) + sinPeak * std::sin(phase);
    double residual = samples[startIndex + n] - fitted;
    residualSumSq += residual * residual;
  }
  metrics.residualRms = std::sqrt(residualSumSq / static_cast<double>(count));
  metrics.sinadDb =
      dbFromRatio(metrics.totalRms / std::max(metrics.residualRms, 1e-20));
  metrics.sndrDb =
      dbFromRatio(metrics.signalRms / std::max(metrics.residualRms, 1e-20));
  return metrics;
}

void fftInPlace(std::vector<std::complex<double>>& bins) {
  const size_t n = bins.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(bins[i], bins[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    double angle = -kRadioTwoPi / static_cast<double>(len);
    std::complex<double> wLen(std::cos(angle), std::sin(angle));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (size_t j = 0; j < len / 2; ++j) {
        std::complex<double> u = bins[i + j];
        std::complex<double> v = bins[i + j + len / 2] * w;
        bins[i + j] = u + v;
        bins[i + j + len / 2] = u - v;
        w *= wLen;
      }
    }
  }
}

std::vector<double> computeSpectrumPeakAmplitudes(const std::vector<float>& samples,
                                                  float sampleRate) {
  std::vector<double> amplitudes(kSpectrumSize / 2 + 1, 0.0);
  if (samples.empty() || sampleRate <= 0.0f) return amplitudes;
  std::vector<std::complex<double>> bins(kSpectrumSize);
  double windowSum = 0.0;
  size_t startIndex = (samples.size() > kSpectrumSize)
                          ? (samples.size() - kSpectrumSize)
                          : 0u;
  for (size_t i = 0; i < kSpectrumSize; ++i) {
    size_t sampleIndex = startIndex + i;
    double x = (sampleIndex < samples.size()) ? samples[sampleIndex] : 0.0;
    double w = 0.5 - 0.5 * std::cos(kRadioTwoPi * static_cast<double>(i) /
                                    static_cast<double>(kSpectrumSize - 1));
    bins[i] = std::complex<double>(x * w, 0.0);
    windowSum += w;
  }
  fftInPlace(bins);
  for (size_t i = 0; i < amplitudes.size(); ++i) {
    double scale = (i == 0 || i + 1 == amplitudes.size()) ? 1.0 : 2.0;
    amplitudes[i] = scale * std::abs(bins[i]) / std::max(windowSum, 1e-12);
  }
  return amplitudes;
}

double peakAmplitudeAtHz(const std::vector<double>& spectrum,
                         float sampleRate,
                         float frequencyHz) {
  if (spectrum.empty() || sampleRate <= 0.0f || frequencyHz < 0.0f) return 0.0;
  double bin = static_cast<double>(kSpectrumSize) * frequencyHz /
               static_cast<double>(sampleRate);
  size_t index = static_cast<size_t>(
      std::clamp<long long>(static_cast<long long>(std::llround(bin)), 0,
                            static_cast<long long>(spectrum.size() - 1)));
  return spectrum[index];
}

double bandRmsFromSpectrum(const std::vector<double>& spectrum,
                           float sampleRate,
                           float lowHz,
                           float highHz) {
  if (spectrum.empty() || sampleRate <= 0.0f || highHz <= lowHz) return 0.0;
  size_t lowBin = static_cast<size_t>(std::floor(
      static_cast<double>(kSpectrumSize) * lowHz / sampleRate));
  size_t highBin = static_cast<size_t>(std::ceil(
      static_cast<double>(kSpectrumSize) * highHz / sampleRate));
  lowBin = std::min(lowBin, spectrum.size() - 1);
  highBin = std::min(highBin, spectrum.size() - 1);
  double sumSq = 0.0;
  for (size_t i = lowBin; i <= highBin; ++i) {
    double rms = spectrum[i] / std::sqrt(2.0);
    sumSq += rms * rms;
  }
  return std::sqrt(sumSq);
}

double meanRange(const std::vector<float>& samples, size_t begin, size_t end) {
  if (begin >= end || begin >= samples.size()) return 0.0;
  end = std::min(end, samples.size());
  double sum = 0.0;
  for (size_t i = begin; i < end; ++i) sum += samples[i];
  return sum / static_cast<double>(end - begin);
}

double findCrossingMs(const std::vector<float>& samples,
                      size_t begin,
                      size_t end,
                      double threshold,
                      bool rising,
                      float sampleRate) {
  if (begin >= end || begin >= samples.size() || sampleRate <= 0.0f) {
    return 0.0;
  }
  end = std::min(end, samples.size());
  for (size_t i = begin; i + 1 < end; ++i) {
    double a = samples[i];
    double b = samples[i + 1];
    if (rising) {
      if (a <= threshold && b >= threshold) {
        double alpha = (threshold - a) / std::max(b - a, 1e-12);
        return 1000.0 * (static_cast<double>(i - begin) + alpha) / sampleRate;
      }
    } else {
      if (a >= threshold && b <= threshold) {
        double alpha = (a - threshold) / std::max(a - b, 1e-12);
        return 1000.0 * (static_cast<double>(i - begin) + alpha) / sampleRate;
      }
    }
  }
  return 0.0;
}

size_t findTransitionCenter(const std::vector<float>& samples,
                            size_t guess,
                            size_t radius) {
  if (samples.size() < 2) return 0u;
  size_t begin = (guess > radius) ? (guess - radius) : 1u;
  size_t end = std::min(guess + radius, samples.size() - 1u);
  size_t best = begin;
  double bestSlope = 0.0;
  for (size_t i = begin; i < end; ++i) {
    double slope = std::fabs(static_cast<double>(samples[i + 1]) - samples[i]);
    if (slope > bestSlope) {
      bestSlope = slope;
      best = i;
    }
  }
  return best;
}

EdgeMetrics measureEnvelopeMetrics(const std::vector<float>& samples,
                                   float sampleRate,
                                   float squareHz = 30.0f) {
  EdgeMetrics metrics;
  metrics.dcShift = measureSignalStats(samples).mean;
  if (samples.size() < 4096 || sampleRate <= 0.0f) return metrics;

  size_t halfPeriod =
      static_cast<size_t>(std::lround(sampleRate / (2.0f * squareHz)));
  if (halfPeriod < 16 || samples.size() < 6 * halfPeriod) return metrics;

  size_t riseGuess = 6 * halfPeriod;
  size_t fallGuess = riseGuess + halfPeriod;
  size_t alignRadius = std::max<size_t>(halfPeriod / 3, 16u);
  size_t riseEdge = findTransitionCenter(samples, riseGuess, alignRadius);
  size_t fallEdge = findTransitionCenter(samples, fallGuess, alignRadius);
  size_t plateau = std::max<size_t>(halfPeriod / 3, 8u);

  double preRise = meanRange(samples, riseEdge - plateau, riseEdge);
  double postRise = meanRange(samples, riseEdge + plateau / 2, riseEdge + plateau + plateau / 2);
  metrics.lowLevel = std::min(preRise, postRise);
  metrics.highLevel = std::max(preRise, postRise);
  bool riseIsAscending = postRise > preRise;
  double delta = std::max(metrics.highLevel - metrics.lowLevel, 1e-9);

  double rise10 = metrics.lowLevel + 0.10 * delta;
  double rise90 = metrics.lowLevel + 0.90 * delta;
  double t10 = findCrossingMs(samples, riseEdge, riseEdge + halfPeriod, rise10,
                              riseIsAscending, sampleRate);
  double t90 = findCrossingMs(samples, riseEdge, riseEdge + halfPeriod, rise90,
                              riseIsAscending, sampleRate);
  metrics.rise10To90Ms = std::max(0.0, t90 - t10);

  size_t overshootBegin = riseEdge + static_cast<size_t>(0.10f * halfPeriod);
  size_t overshootEnd = std::min(riseEdge + halfPeriod, samples.size());
  double riseExtreme = riseIsAscending ? -std::numeric_limits<double>::infinity()
                                       : std::numeric_limits<double>::infinity();
  for (size_t i = overshootBegin; i < overshootEnd; ++i) {
    riseExtreme = riseIsAscending ? std::max(riseExtreme, static_cast<double>(samples[i]))
                                  : std::min(riseExtreme, static_cast<double>(samples[i]));
  }
  metrics.overshoot = riseIsAscending
                          ? std::max(0.0, (riseExtreme - metrics.highLevel) / delta)
                          : std::max(0.0, (metrics.lowLevel - riseExtreme) / delta);

  double preFall = meanRange(samples, fallEdge - plateau, fallEdge);
  double postFall =
      meanRange(samples, fallEdge + plateau / 2, fallEdge + plateau + plateau / 2);
  bool fallIsDescending = postFall < preFall;
  double fall90 = metrics.lowLevel + 0.90 * delta;
  double fall10 = metrics.lowLevel + 0.10 * delta;
  double tf90 = findCrossingMs(samples, fallEdge, fallEdge + halfPeriod, fall90,
                               !fallIsDescending, sampleRate);
  double tf10 = findCrossingMs(samples, fallEdge, fallEdge + halfPeriod, fall10,
                               !fallIsDescending, sampleRate);
  metrics.fall90To10Ms = std::max(0.0, tf10 - tf90);

  size_t undershootBegin = fallEdge + static_cast<size_t>(0.10f * halfPeriod);
  size_t undershootEnd = std::min(fallEdge + halfPeriod, samples.size());
  double fallExtreme = fallIsDescending ? std::numeric_limits<double>::infinity()
                                        : -std::numeric_limits<double>::infinity();
  for (size_t i = undershootBegin; i < undershootEnd; ++i) {
    fallExtreme =
        fallIsDescending ? std::min(fallExtreme, static_cast<double>(samples[i]))
                         : std::max(fallExtreme, static_cast<double>(samples[i]));
  }
  metrics.undershoot =
      fallIsDescending
          ? std::max(0.0, (metrics.lowLevel - fallExtreme) / delta)
          : std::max(0.0, (fallExtreme - metrics.highLevel) / delta);
  return metrics;
}

std::vector<float> makeSine(float sampleRate,
                            uint32_t frames,
                            float frequencyHz,
                            float peak) {
  std::vector<float> samples(frames, 0.0f);
  for (uint32_t i = 0; i < frames; ++i) {
    float t = static_cast<float>(i) / sampleRate;
    samples[i] = peak * std::sin(kRadioTwoPi * frequencyHz * t);
  }
  return samples;
}

std::vector<float> makeTwoTone(float sampleRate,
                               uint32_t frames,
                               float f1,
                               float f2,
                               float peak1,
                               float peak2) {
  std::vector<float> samples(frames, 0.0f);
  for (uint32_t i = 0; i < frames; ++i) {
    float t = static_cast<float>(i) / sampleRate;
    samples[i] = peak1 * std::sin(kRadioTwoPi * f1 * t) +
                 peak2 * std::sin(kRadioTwoPi * f2 * t);
  }
  return samples;
}

std::vector<float> makeSquare(float sampleRate,
                              uint32_t frames,
                              float frequencyHz,
                              float peak) {
  std::vector<float> samples(frames, 0.0f);
  for (uint32_t i = 0; i < frames; ++i) {
    float t = static_cast<float>(i) / sampleRate;
    samples[i] = (std::sin(kRadioTwoPi * frequencyHz * t) >= 0.0f) ? peak : -peak;
  }
  return samples;
}

float selectHarnessCarrierHz(const HarnessConfig& config, float noiseWeight) {
  Radio1938 radio = makeRadio(config, noiseWeight, false);
  return std::clamp(radio.ifStrip.sourceCarrierHz, 1000.0f,
                    std::max(config.sampleRate * 0.45f, 1000.0f));
}

std::vector<float> makeCarrierBurstRf(float sampleRate,
                                      uint32_t frames,
                                      float carrierHz,
                                      float carrierRmsVolts,
                                      float burstHz) {
  std::vector<float> samples(frames, 0.0f);
  float carrierPeak = std::sqrt(2.0f) * std::max(carrierRmsVolts, 0.0f);
  for (uint32_t i = 0; i < frames; ++i) {
    float t = static_cast<float>(i) / sampleRate;
    float gate = (std::sin(kRadioTwoPi * burstHz * t) >= 0.0f) ? 1.0f : 0.0f;
    samples[i] = gate * carrierPeak * std::cos(kRadioTwoPi * carrierHz * t);
  }
  return samples;
}

std::vector<float> makeSilence(uint32_t frames) {
  return std::vector<float>(frames, 0.0f);
}

void printConfig(const HarnessConfig& config) {
  const auto radio = makeRadio(config, config.noiseWeight, false);
  std::cout << "[config]\n";
  std::cout << "key,value\n";
  // Report resolved state, after the overlay, rather than echoing CLI intent.
  std::cout << "receiver_profile," << radioReceiverProfileName(radio.receiverProfile) << "\n";
  std::cout << "first_audio_mu," << radio.receiverCircuit.tubeMu << "\n";
  std::cout << "oversample_factor," << radio.globals.oversampleFactor << "\n";
  std::cout << "nominal_output_power_watts," << radio.power.nominalOutputPowerWatts << "\n";
  std::cout << "sample_rate_hz," << config.sampleRate << "\n";
  std::cout << "bandwidth_hz," << config.bandwidthHz << "\n";
  std::cout << "carrier_rms_volts," << config.carrierRmsVolts << "\n";
  std::cout << "modulation_index," << config.modulationIndex << "\n";
  std::cout << "noise_weight," << config.noiseWeight << "\n";
  std::cout << "noise_only_weight," << config.noiseOnlyWeight << "\n";
  std::cout << "speaker_natural_hz_override," << config.speakerNaturalHzOverride << "\n";
  std::cout << "detector_waveform_spc_override,"
            << config.detectorWaveformSamplesPerCycle << "\n";
  std::cout << "detector_waveform_max_substeps_override,"
            << config.detectorWaveformMaxSubsteps << "\n\n";
}

bool wantsSection(const HarnessConfig& config, std::string_view section) {
  return config.section == "all" || config.section == section;
}

std::vector<SweepPoint> measureSweepPoints(const HarnessConfig& config) {
  std::vector<SweepPoint> points;
  points.reserve(kSweepFrequenciesHz.size());
  for (float hz : kSweepFrequenciesHz) {
    auto input = makeSine(config.sampleRate, kSteadyTestFrames, hz,
                          kSweepProgramPeak);
    RunResult run = runAmProgram(config, input, config.modulationIndex,
                                 config.noiseWeight);
    size_t analysisStart = static_cast<size_t>(config.sampleRate * 0.10f);
    SweepPoint point;
    point.frequencyHz = hz;
    point.inputRms = measureToneRms(input, config.sampleRate, hz, analysisStart);
    point.outputRms =
        measureToneRms(run.output, config.sampleRate, hz, analysisStart);
    point.gainDb =
        dbFromRatio(point.outputRms / std::max(point.inputRms, 1e-12));
    point.dcShift = measureSignalStats(run.output, analysisStart).mean;
    points.push_back(point);
  }
  return points;
}

SweepSummary summarizeSweep(const std::vector<SweepPoint>& points) {
  SweepSummary summary;
  if (points.empty()) return summary;
  for (const auto& point : points) {
    summary.peakGainDb = std::max(summary.peakGainDb, point.gainDb);
  }
  double thresholdDb = summary.peakGainDb - 3.0;
  for (const auto& point : points) {
    if (point.gainDb >= thresholdDb) {
      if (summary.low3dBHz <= 0.0) summary.low3dBHz = point.frequencyHz;
      summary.high3dBHz = point.frequencyHz;
    }
  }
  return summary;
}

double measureImdSummaryDb(const HarnessConfig& config) {
  auto input = makeTwoTone(config.sampleRate, kSteadyTestFrames, 400.0f, 2000.0f,
                           kImdTonePeak, kImdTonePeak);
  RunResult run = runAmProgram(config, input, config.modulationIndex,
                               config.noiseWeight);
  std::vector<double> spectrum =
      computeSpectrumPeakAmplitudes(run.output, config.sampleRate);
  double h400 = peakAmplitudeAtHz(spectrum, config.sampleRate, 400.0f);
  double h2000 = peakAmplitudeAtHz(spectrum, config.sampleRate, 2000.0f);
  double h1600 = peakAmplitudeAtHz(spectrum, config.sampleRate, 1600.0f);
  double h2400 = peakAmplitudeAtHz(spectrum, config.sampleRate, 2400.0f);
  double h1200 = peakAmplitudeAtHz(spectrum, config.sampleRate, 1200.0f);
  double h2800 = peakAmplitudeAtHz(spectrum, config.sampleRate, 2800.0f);
  double fundamentalPower = h400 * h400 + h2000 * h2000;
  double imdPower = h1600 * h1600 + h2400 * h2400 + h1200 * h1200 + h2800 * h2800;
  return dbFromRatio(std::sqrt(imdPower / std::max(fundamentalPower, 1e-20)));
}

ReferenceAnchorSummary measureReferenceAnchorSummary(const HarnessConfig& config) {
  HarnessConfig nominalConfig = config;
  nominalConfig.carrierRmsVolts = kDefaultCarrierRmsVolts;
  auto nominalInput =
      makeSine(config.sampleRate, kSteadyTestFrames, 1000.0f,
               kNominalProgramPeak);
  RunResult nominalRun = runAmProgram(
      nominalConfig, nominalInput, nominalHarnessModulationIndex(config),
      std::max(config.noiseWeight, config.noiseOnlyWeight));
  ToneMetrics nominalTone = measureToneMetrics(
      nominalRun.output, config.sampleRate, 1000.0f,
      static_cast<size_t>(config.sampleRate * 0.20f));
  ReferenceAnchorSummary summary;
  summary.sinadNominalDb = nominalTone.sinadDb;
  summary.speakerReferenceRatio =
      nominalRun.radio.calibration.speakerReferenceRmsRatio;
  summary.maxDigitalOutput = nominalRun.radio.calibration.maxDigitalOutput;
  summary.waveformSubsteps = nominalRun.radio.demod.am.lastWaveformSubsteps;
  return summary;
}

AmValidationSummary measureAmValidationSummary(const HarnessConfig& config) {
  auto program =
      makeSine(config.sampleRate, kSteadyTestFrames, 1000.0f, kSweepProgramPeak);
  float modulationIndex = std::min(config.modulationIndex, 0.999f);
  float effectiveNoiseWeight =
      std::max(config.noiseWeight, config.noiseOnlyWeight);
  EnvelopeTraceResult trace = runAmProgramWithEnvelopeTrace(
      config, program, modulationIndex, effectiveNoiseWeight);
  RunResult directRun =
      runAmProgram(config, program, modulationIndex, effectiveNoiseWeight);

  AmValidationSummary summary;
  summary.modulationIndexConfig = modulationIndex;
  summary.waveformSubsteps = trace.radio.demod.am.lastWaveformSubsteps;
  if (effectiveNoiseWeight <= 0.0f) {
    for (size_t i = 0; i < directRun.output.size() && i < trace.output.size();
         ++i) {
      summary.maxOutputAbsDiff =
          std::max(summary.maxOutputAbsDiff,
                   std::fabs(static_cast<double>(directRun.output[i]) -
                             static_cast<double>(trace.output[i])));
    }
  }

  float safeSampleRate = std::max(config.sampleRate, 1.0f);
  float carrierHz = trace.radio.resolvedInputCarrierHz();
  float carrierPeak = std::sqrt(2.0f) * std::max(config.carrierRmsVolts, 0.0f);
  std::vector<float> rfSamples(program.size(), 0.0f);
  float normScale = amProgramNormScale(program);
  double programPeakAfterNorm = 0.0;
  for (float sample : program) {
    programPeakAfterNorm =
        std::max(programPeakAfterNorm,
                 std::fabs(static_cast<double>(sample * normScale)));
  }
  summary.effectiveModulationDepth = modulationIndex * programPeakAfterNorm;
  for (size_t i = 0; i < program.size(); ++i) {
    float t = static_cast<float>(i) / safeSampleRate;
    float sampleVal = program[i] * normScale;
    float envelopeFactor = std::max(0.0f, 1.0f + modulationIndex * sampleVal);
    rfSamples[i] =
        carrierPeak * envelopeFactor * std::cos(kRadioTwoPi * carrierHz * t);
  }

  summary.carrierRms =
      measureToneRms(rfSamples, config.sampleRate, carrierHz, 0);
  summary.sideUpperRms =
      measureToneRms(rfSamples, config.sampleRate, carrierHz + 1000.0f, 0);
  summary.sideLowerRms =
      measureToneRms(rfSamples, config.sampleRate, carrierHz - 1000.0f, 0);
  summary.expectedSideRms =
      (summary.effectiveModulationDepth * 0.5) * summary.carrierRms;
  double sideAvgRms = 0.5 * (summary.sideUpperRms + summary.sideLowerRms);
  summary.sidebandRatio =
      sideAvgRms / std::max(summary.expectedSideRms, 1e-20);
  summary.sidebandRatioInBand =
      summary.sidebandRatio >= kAmSidebandRatioMin &&
      summary.sidebandRatio <= kAmSidebandRatioMax;

  std::vector<double> detSpec =
      computeSpectrumPeakAmplitudes(trace.detectorNode, config.sampleRate);
  summary.detectorDc = (detSpec.size() > 0) ? detSpec[0] : 0.0;
  summary.detectorTone =
      peakAmplitudeAtHz(detSpec, config.sampleRate, 1000.0f);
  summary.detectorToneToDcRatio =
      summary.detectorTone / std::max(summary.detectorDc, 1e-20);

  double envMax = -1e300;
  double envMin = 1e300;
  size_t start = program.size() / 4;
  for (size_t i = start; i < trace.audioEnv.size(); ++i) {
    envMax = std::max(envMax, static_cast<double>(trace.audioEnv[i]));
    envMin = std::min(envMin, static_cast<double>(trace.audioEnv[i]));
  }
  if (envMax + envMin > 1e-12) {
    summary.modulationIndexMeasuredEnv = (envMax - envMin) / (envMax + envMin);
  }
  summary.modulationIndexInBand =
      std::fabs(summary.modulationIndexMeasuredEnv -
                summary.effectiveModulationDepth) <= kAmModIndexTolerance;

  ToneMetrics detToneMetrics =
      measureToneMetrics(trace.detectorNode, config.sampleRate, 1000.0f, start);
  ToneMetrics outToneMetrics =
      measureToneMetrics(trace.output, config.sampleRate, 1000.0f, start);
  summary.detectorSinadDb = detToneMetrics.sinadDb;
  summary.outputSinadDb = outToneMetrics.sinadDb;
  summary.speakerReferenceRatio =
      trace.radio.calibration.speakerReferenceRmsRatio;
  summary.maxDigitalOutput = trace.radio.calibration.maxDigitalOutput;
  summary.ifCenterHz = trace.radio.ifStrip.ifCenterHz;
  summary.nominalOutputPowerWatts = trace.radio.power.nominalOutputPowerWatts;
  return summary;
}

void runSweep(const HarnessConfig& config) {
  std::cout << "[sweep]\n";
  std::cout << "freq_hz,input_rms,output_rms,gain_db,dc_shift\n";
  for (const auto& point : measureSweepPoints(config)) {
    std::cout << point.frequencyHz << "," << point.inputRms << ","
              << point.outputRms << "," << point.gainDb << ","
              << point.dcShift << "\n";
  }
  std::cout << "\n";
}

void runEnvelope(const HarnessConfig& config) {
  std::cout << "[envelope]\n";
  std::cout << "metric,value\n";
  auto input = makeSquare(config.sampleRate, kEnvelopeTestFrames, 30.0f, 0.85f);
  EnvelopeTraceResult run = runAmProgramWithEnvelopeTrace(
      config, input, config.modulationIndex, config.noiseWeight);
  EdgeMetrics metrics =
      measureEnvelopeMetrics(run.audioEnv, config.sampleRate, 30.0f);
  std::cout << "trace,detector_audio_env\n";
  std::cout << "low_level," << metrics.lowLevel << "\n";
  std::cout << "high_level," << metrics.highLevel << "\n";
  std::cout << "rise_10_90_ms," << metrics.rise10To90Ms << "\n";
  std::cout << "fall_90_10_ms," << metrics.fall90To10Ms << "\n";
  std::cout << "overshoot_fraction," << metrics.overshoot << "\n";
  std::cout << "undershoot_fraction," << metrics.undershoot << "\n";
  std::cout << "dc_shift," << metrics.dcShift << "\n\n";
}

void runLevels(const HarnessConfig& config) {
  std::cout << "[levels]\n";
  std::cout << "metric,value\n";
  auto input =
      makeSine(config.sampleRate, kSteadyTestFrames, 1000.0f,
               kNominalProgramPeak);
  RunResult run = runAmProgram(config, input, config.modulationIndex,
                               config.noiseWeight);
  std::cout << "detector_node_rms," << run.radio.calibration.detectorNodeVolts.rms
            << "\n";
  std::cout << "receiver_grid_rms," << run.radio.calibration.receiverGridVolts.rms
            << "\n";
  std::cout << "tone_out_rms,"
            << run.radio.calibration
                   .passes[static_cast<size_t>(PassId::Tone)]
                   .rmsOut
            << "\n";
  std::cout << "driver_grid_rms," << run.radio.calibration.driverGridVolts.rms
            << "\n";
  std::cout << "output_grid_a_rms,"
            << run.radio.calibration.outputGridAVolts.rms << "\n";
  std::cout << "output_grid_b_rms,"
            << run.radio.calibration.outputGridBVolts.rms << "\n";
  std::cout << "output_primary_rms,"
            << run.radio.calibration.outputPrimaryVolts.rms << "\n";
  std::cout << "speaker_secondary_rms,"
            << run.radio.calibration.speakerSecondaryVolts.rms << "\n";
  std::cout << "speaker_secondary_peak,"
            << run.radio.calibration.maxSpeakerSecondaryVolts << "\n";
  std::cout << "speaker_reference_ratio,"
            << run.radio.calibration.speakerReferenceRmsRatio << "\n";
  std::cout << "speaker_peak_reference_ratio,"
            << run.radio.calibration.maxSpeakerReferenceRatio << "\n";
  std::cout << "detector_waveform_avg_solve_steps,"
            << run.radio.calibration.detectorWaveformAverageSolveSteps << "\n";
  std::cout << "detector_waveform_split_fraction,"
            << run.radio.calibration.detectorWaveformSplitFraction << "\n";
  std::cout << "detector_storage_avg_iterations,"
            << run.radio.calibration.detectorStorageAverageIterations << "\n";
  std::cout << "detector_storage_max_iterations,"
            << run.radio.calibration.detectorStorageMaxIterations << "\n";
  std::cout << "detector_afc_probe_ms,"
            << run.radio.calibration.detectorAfcProbeMs << "\n";
  std::cout << "detector_afc_probe_us_per_sample,"
            << run.radio.calibration.detectorAfcProbeUsPerSample << "\n";
  std::cout << "detector_island_ms,"
            << run.radio.calibration.detectorIslandMs << "\n";
  std::cout << "detector_island_us_per_sample,"
            << run.radio.calibration.detectorIslandUsPerSample << "\n";
  std::cout << "detector_storage_solve_ms,"
            << run.radio.calibration.detectorStorageSolveMs << "\n";
  std::cout << "detector_storage_solve_us_per_call,"
            << run.radio.calibration.detectorStorageSolveUsPerCall << "\n";
  std::cout << "power_output_avg_newton_iterations,"
            << run.radio.calibration.powerOutputAverageNewtonIterations << "\n";
  std::cout << "power_output_max_newton_iterations,"
            << run.radio.calibration.powerOutputMaxNewtonIterations << "\n";
  std::cout << "power_output_avg_transformer_substeps,"
            << run.radio.calibration.powerOutputAverageTransformerSubsteps
            << "\n";
  std::cout << "power_output_solve_ms,"
            << run.radio.calibration.powerOutputSolveMs << "\n";
  std::cout << "power_output_solve_us_per_sample,"
            << run.radio.calibration.powerOutputSolveUsPerSample << "\n";
  std::cout << "power_interstage_avg_iterations,"
            << run.radio.calibration.powerInterstageAverageIterations << "\n";
  std::cout << "power_interstage_max_iterations,"
            << run.radio.calibration.powerInterstageMaxIterations << "\n";
  std::cout << "power_interstage_avg_substeps,"
            << run.radio.calibration.powerInterstageAverageSubsteps << "\n";
  std::cout << "power_interstage_solve_ms,"
            << run.radio.calibration.powerInterstageSolveMs << "\n";
  std::cout << "power_interstage_solve_us_per_sample,"
            << run.radio.calibration.powerInterstageSolveUsPerSample << "\n";
  std::cout << "power_interstage_driver_eval_calls_per_sample,"
            << run.radio.calibration.powerInterstageDriverEvalCallsPerSample
            << "\n";
  std::cout << "power_interstage_adaptive_validation_attempts,"
            << run.radio.calibration.powerInterstageAdaptiveValidationAttempts
            << "\n";
  std::cout << "power_interstage_adaptive_accepted_downshifts,"
            << run.radio.calibration.powerInterstageAdaptiveAcceptedDownshifts
            << "\n";
  std::cout << "power_interstage_adaptive_accepted_downshift_fraction,"
            << run.radio.calibration
                   .powerInterstageAdaptiveAcceptedDownshiftFraction
            << "\n";
  std::cout << "power_interstage_adaptive_avg_boundary_error_volts,"
            << run.radio.calibration
                   .powerInterstageAdaptiveAverageBoundaryErrorVolts
            << "\n";
  std::cout << "power_interstage_adaptive_max_boundary_error_volts,"
            << run.radio.calibration
                   .powerInterstageAdaptiveMaxBoundaryErrorVolts
            << "\n";
  std::cout << "digital_reference_peak,"
            << run.radio.output.digitalReferenceSpeakerVoltsPeak << "\n";
  std::cout << "max_digital_output," << run.radio.calibration.maxDigitalOutput
            << "\n";
  std::cout << "power_clip_samples," << run.radio.diagnostics.powerClipSamples
            << "\n";
  std::cout << "speaker_clip_samples,"
            << run.radio.diagnostics.speakerClipSamples << "\n";
  std::cout << "output_clip_samples," << run.radio.diagnostics.outputClipSamples
            << "\n";
  std::cout << "final_limiter_active_samples,"
            << run.radio.diagnostics.finalLimiterActiveSamples << "\n";
  std::cout << "validation_speaker_over_reference,"
            << (run.radio.calibration.validationSpeakerOverReference ? 1 : 0)
            << "\n";
  std::cout << "validation_digital_clip,"
            << (run.radio.calibration.validationDigitalClip ? 1 : 0) << "\n\n";
}

void runPowerBench(const HarnessConfig& config) {
  std::cout << "[power_bench]\n";
  std::cout << "input_peak,secondary_rms,secondary_peak,current_rms,"
               "load_real_ohms,load_imag_ohms,load_mag_ohms,"
               "reflected_load_real_ohms,reflected_load_mag_ohms,"
               "sndr_db,thd_db,grid_a_positive_fraction,"
               "grid_b_positive_fraction,power_clip\n";
  constexpr float kPowerBenchHz = 1000.0f;
  const size_t analysisStart = static_cast<size_t>(config.sampleRate * 0.20f);
  for (float inputPeak : kPowerBenchInputPeaks) {
    auto input =
        makeSine(config.sampleRate, kSteadyTestFrames, kPowerBenchHz, inputPeak);
    PowerOnlyTraceResult run = runPowerOnlyProgram(config, input);
    ToneMetrics tone =
        measureToneMetrics(run.output, config.sampleRate, kPowerBenchHz, analysisStart);
    std::complex<double> voltagePhasor =
        measureTonePhasor(run.output, config.sampleRate, kPowerBenchHz, analysisStart);
    std::complex<double> currentPhasor = measureTonePhasor(
        run.speakerCurrent, config.sampleRate, kPowerBenchHz, analysisStart);
    std::complex<double> impedance =
        (std::abs(currentPhasor) > 1e-12) ? (voltagePhasor / currentPhasor)
                                          : std::complex<double>(0.0, 0.0);
    double turns = std::max<double>(
        run.radio.power.outputTransformerTurnsRatioPrimaryToSecondary, 1e-6);
    double turnsSq = turns * turns;
    std::vector<double> spectrum =
        computeSpectrumPeakAmplitudes(run.output, config.sampleRate);
    double h1 = peakAmplitudeAtHz(spectrum, config.sampleRate, 1000.0f);
    double h2 = peakAmplitudeAtHz(spectrum, config.sampleRate, 2000.0f);
    double h3 = peakAmplitudeAtHz(spectrum, config.sampleRate, 3000.0f);
    double h4 = peakAmplitudeAtHz(spectrum, config.sampleRate, 4000.0f);
    double h5 = peakAmplitudeAtHz(spectrum, config.sampleRate, 5000.0f);
    double harmonicRms = std::sqrt(h2 * h2 + h3 * h3 + h4 * h4 + h5 * h5);
    double thdDb = dbFromRatio(harmonicRms / std::max(h1, 1e-20));

    std::cout << inputPeak << ","
              << measureToneRms(run.output, config.sampleRate, kPowerBenchHz,
                                analysisStart)
              << "," << measureSignalStats(run.output, analysisStart).peak << ","
              << measureToneRms(run.speakerCurrent, config.sampleRate,
                                kPowerBenchHz, analysisStart)
              << "," << impedance.real() << "," << impedance.imag() << ","
              << std::abs(impedance) << "," << (turnsSq * impedance.real()) << ","
              << (turnsSq * std::abs(impedance)) << "," << tone.sndrDb << ","
              << thdDb << ","
              << run.radio.calibration.outputGridAPositiveFraction << ","
              << run.radio.calibration.outputGridBPositiveFraction << ","
              << (run.radio.diagnostics.powerClip ? 1 : 0) << "\n";
  }
  std::cout << "\n";
}

void runImd(const HarnessConfig& config) {
  std::cout << "[imd]\n";
  std::cout << "component_hz,peak_amp,dbfs\n";
  auto input = makeTwoTone(config.sampleRate, kSteadyTestFrames, 400.0f, 2000.0f,
                           kImdTonePeak, kImdTonePeak);
  RunResult run = runAmProgram(config, input, config.modulationIndex,
                               config.noiseWeight);
  std::vector<double> spectrum =
      computeSpectrumPeakAmplitudes(run.output, config.sampleRate);
  std::array<float, 6> freqs = {400.0f, 2000.0f, 1600.0f, 2400.0f, 1200.0f, 2800.0f};
  double fundamentalPower = 0.0;
  double imdPower = 0.0;
  for (float hz : freqs) {
    double amp = peakAmplitudeAtHz(spectrum, config.sampleRate, hz);
    if (hz == 400.0f || hz == 2000.0f) {
      fundamentalPower += amp * amp;
    } else {
      imdPower += amp * amp;
    }
    std::cout << hz << "," << amp << "," << dbFromRatio(amp) << "\n";
  }
  std::cout << "imd_summary,"
            << std::sqrt(imdPower / std::max(fundamentalPower, 1e-20)) << ","
            << dbFromRatio(std::sqrt(imdPower / std::max(fundamentalPower, 1e-20)))
            << "\n\n";
}

void runSpectrum(const HarnessConfig& config) {
  std::cout << "[spectrum]\n";
  std::cout << "freq_hz,peak_amp,dbfs\n";
  // Nominal run to get output waveform
  auto input =
      makeSine(config.sampleRate, kSteadyTestFrames, 1000.0f,
               kNominalProgramPeak);
  // Use the envelope-trace run to also capture detector node and audioEnv
  EnvelopeTraceResult trace = runAmProgramWithEnvelopeTrace(
      config, input, nominalHarnessModulationIndex(config),
      std::max(config.noiseWeight, config.noiseOnlyWeight));

  // Detector node spectrum
  std::cout << "[detector_spectrum]\n";
  std::cout << "freq_hz,peak_amp,dbfs\n";
  std::vector<double> detSpec = computeSpectrumPeakAmplitudes(trace.detectorNode, config.sampleRate);
  for (size_t i = 0; i < detSpec.size(); ++i) {
    double hz = static_cast<double>(i) * static_cast<double>(config.sampleRate) / static_cast<double>(kSpectrumSize);
    double amp = detSpec[i];
    double db = dbFromRatio(amp);
    std::cout << hz << "," << amp << "," << db << "\n";
  }
  std::cout << "\n";

  // Final output spectrum
  std::cout << "[output_spectrum]\n";
  std::cout << "freq_hz,peak_amp,dbfs\n";
  std::vector<double> outSpec = computeSpectrumPeakAmplitudes(trace.output, config.sampleRate);
  for (size_t i = 0; i < outSpec.size(); ++i) {
    double hz = static_cast<double>(i) * static_cast<double>(config.sampleRate) / static_cast<double>(kSpectrumSize);
    double amp = outSpec[i];
    double db = dbFromRatio(amp);
    std::cout << hz << "," << amp << "," << db << "\n";
  }
  std::cout << "\n";

  // Print top peaks summary for quick inspection
  auto printTopPeaks = [&](const std::vector<double>& spec, const std::string& label) {
    std::vector<size_t> idx(spec.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return spec[a] > spec[b]; });
    std::cout << "[" << label << " top_peaks]\n";
    std::cout << "rank,bin,freq_hz,amp,dbfs\n";
    size_t topN = std::min<size_t>(8, idx.size());
    for (size_t r = 0; r < topN; ++r) {
      size_t bin = idx[r];
      double hz = static_cast<double>(bin) * static_cast<double>(config.sampleRate) / static_cast<double>(kSpectrumSize);
      double amp = spec[bin];
      double db = dbFromRatio(amp);
      std::cout << (r + 1) << "," << bin << "," << hz << "," << amp << "," << db << "\n";
    }
    std::cout << "\n";
  };

  printTopPeaks(detSpec, "detector");
  printTopPeaks(outSpec, "output");
}

void runOverdrive(const HarnessConfig& config) {
  std::cout << "[overdrive]\n";
  std::cout << "mod_index,max_digital,pos_peak,neg_peak,peak_asymmetry,dc_shift,"
               "h2_dbc,h3_dbc,h4_dbc,h5_dbc,thd_db,speaker_ref_ratio,"
               "power_clip,speaker_clip,output_clip,final_limiter_active,"
               "validation_digital_clip,validation_failed\n";
  const float f = config.toneHz;
  auto input = makeSine(config.sampleRate, kSteadyTestFrames, f,
                        kOverdriveProgramPeak);
  constexpr std::array<float, 8> kModIndices = {0.10f, 0.25f, 0.50f, 0.75f,
                                                0.90f, 1.00f, 1.10f, 1.20f};
  for (float modIndex : kModIndices) {
    RunResult run = runAmProgram(config, input, modIndex, config.noiseWeight);
    SignalStats stats =
        measureSignalStats(run.output, static_cast<size_t>(config.sampleRate * 0.20f));
    std::vector<double> spectrum =
        computeSpectrumPeakAmplitudes(run.output, config.sampleRate);
    double h1 = peakAmplitudeAtHz(spectrum, config.sampleRate, f);
    double h2 = peakAmplitudeAtHz(spectrum, config.sampleRate, 2 * f);
    double h3 = peakAmplitudeAtHz(spectrum, config.sampleRate, 3 * f);
    double h4 = peakAmplitudeAtHz(spectrum, config.sampleRate, 4 * f);
    double h5 = peakAmplitudeAtHz(spectrum, config.sampleRate, 5 * f);
    double harmonicRms = std::sqrt(h2 * h2 + h3 * h3 + h4 * h4 + h5 * h5);
    double thd = harmonicRms / std::max(h1, 1e-20);
    double asymmetry =
        (std::fabs(stats.posPeak) - std::fabs(stats.negPeak)) /
        std::max(stats.peak, 1e-12);
    std::cout << modIndex << "," << run.radio.calibration.maxDigitalOutput << ","
              << stats.posPeak << "," << stats.negPeak << "," << asymmetry
              << "," << stats.mean << "," << dbFromRatio(h2 / std::max(h1, 1e-20))
              << "," << dbFromRatio(h3 / std::max(h1, 1e-20)) << ","
              << dbFromRatio(h4 / std::max(h1, 1e-20)) << ","
              << dbFromRatio(h5 / std::max(h1, 1e-20)) << ","
              << dbFromRatio(thd) << ","
              << run.radio.calibration.speakerReferenceRmsRatio << ","
              << (run.radio.diagnostics.powerClip ? 1 : 0) << ","
              << (run.radio.diagnostics.speakerClip ? 1 : 0) << ","
              << (run.radio.diagnostics.outputClip ? 1 : 0) << ","
              << (run.radio.diagnostics.finalLimiterActive ? 1 : 0) << ","
              << (run.radio.calibration.validationDigitalClip ? 1 : 0) << ","
              << (run.radio.calibration.validationFailed ? 1 : 0) << "\n";
  }
  std::cout << "\n";
}

// Static AVC characteristic: speaker-terminal audio and AVC line voltage for a
// 1 kHz, 30% modulated carrier from 0.3 mV to 1 V RMS at the antenna.
void runAvcCharacteristic(const HarnessConfig& config) {
  std::cout << "[avc]\n";
  std::cout << "carrier_rms,carrier_dbv,terminal_audio_rms,audio_db_re_nominal,"
               "avc_line_volts,detector_volts\n";
  constexpr std::array<float, 9> kCarriers = {0.0003f, 0.001f, 0.003f, 0.01f, 0.03f,
                                              0.12f,   0.3f,   1.0f,   3.0f};
  const auto input = makeSine(config.sampleRate, kSteadyTestFrames, 1000.0f, 1.0f);
  const size_t start = static_cast<size_t>(config.sampleRate * 0.25f);
  HarnessConfig levelConfig = config;
  levelConfig.observeTerminalVoltage = true;
  levelConfig.carrierRmsVolts = kDefaultCarrierRmsVolts;
  const double nominal =
      measureSignalStats(runAmProgram(levelConfig, input, .3f, 0).output, start).rms;
  for (float carrier : kCarriers) {
    levelConfig.carrierRmsVolts = carrier;
    RunResult run = runAmProgram(levelConfig, input, .3f, 0);
    const double audio = measureSignalStats(run.output, start).rms;
    std::cout << carrier << "," << dbFromRatio(carrier) << "," << audio << ","
              << dbFromRatio(audio / std::max(nominal, 1e-20)) << ","
              << run.radio.demod.am.avcEnv << "," << run.radio.demod.am.detectorNode
              << "\n";
  }
  std::cout << "\n";
}

// IRE/RMA sensitivity: the least carrier, 30% modulated at 400 Hz, that gives
// the 50 mW standard output into the rated voice-coil load with the volume
// control fully up and the AVC working (Radio News, April 1937; IRE 1948
// standard 2.04 and 2.09.02). The carrier is the antenna EMF behind the
// receiver's own antenna circuit. Only the 400 Hz component counts.
double measureStandardOutputWatts(const HarnessConfig& config, float carrierRms) {
  HarnessConfig levelConfig = config;
  levelConfig.observeTerminalVoltage = true;
  levelConfig.carrierRmsVolts = carrierRms;
  levelConfig.volumeOverride = 1.0f;
  const auto input = makeSine(config.sampleRate, kSteadyTestFrames, 400.0f, 1.0f);
  RunResult run = runAmProgram(levelConfig, input, .3f, 0);
  const size_t start = static_cast<size_t>(config.sampleRate * 0.25f);
  std::vector<float> tail(run.output.begin() + start, run.output.end());
  const auto spectrum = computeSpectrumPeakAmplitudes(tail, config.sampleRate);
  const double peak = peakAmplitudeAtHz(spectrum, config.sampleRate, 400.0f);
  const double load = run.radio.speakerStage.speaker.nominalLoadOhms;
  if (!(load > 0)) fail("sensitivity needs the rated voice-coil load");
  return .5 * peak * peak / load;
}

void runSensitivity(const HarnessConfig& config) {
  std::cout << "[sensitivity]\n";
  constexpr double kStandardOutputWatts = .05;
  double low = 1e-7, high = 1.0;
  if (measureStandardOutputWatts(config, static_cast<float>(high)) < kStandardOutputWatts)
    fail("receiver cannot reach the 50 mW standard output");
  for (int step = 0; step < 24; ++step) {
    const double mid = std::sqrt(low * high);
    (measureStandardOutputWatts(config, static_cast<float>(mid)) < kStandardOutputWatts
         ? low : high) = mid;
  }
  const double sensitivity = std::sqrt(low * high);
  // Gain chain without AVC: the converter's conversion transconductance and
  // the IF tubes' gm, from the tube data at their operating points.
  Radio1938 probe = makeRadio(config, 0, false);
  probe.controlBus.controlVoltage = 0;
  RadioSampleContext probeCtx{};
  RadioMixerNode::run(probe, 0, probeCtx);
  std::cout << "metric,value\n";
  std::cout << "conversion_transconductance_siemens," << probe.mixer.conversionGain << "\n";
  std::cout << "if_stage_transconductance_siemens,"
            << probe.ifStrip.stageTransconductanceSiemens << "\n";
  std::cout << "avc_rectifier_per_if_volt," << probe.demod.am.avcRectifierPerIfVolt << "\n";
  std::cout << "standard_output_watts," << kStandardOutputWatts << "\n";
  std::cout << "sensitivity_microvolts," << 1e6 * sensitivity << "\n";
  std::cout << "sensitivity_dbv," << dbFromRatio(sensitivity) << "\n\n";
}

// Recovery from one lightning crash: an RF impulse of the given area (volt-
// seconds at the antenna) on an unmodulated carrier. A receiver must return
// to its working point once the AVC has recovered.
void runCrashRecovery(const HarnessConfig& config) {
  std::cout << "[crash]\n";
  std::cout << "time_ms,avc_line_volts,detector_volts,terminal_volts\n";
  HarnessConfig levelConfig = config;
  Radio1938 radio = makeRadio(levelConfig, 0, true);
  const uint32_t frames = static_cast<uint32_t>(config.sampleRate * 1.5f);
  std::vector<float> input(frames, 0.0f), output(frames), terminal(frames);
  std::vector<RadioAmReceptionSample> paths(frames);
  const uint32_t at = static_cast<uint32_t>(config.sampleRate * 0.1f);
  paths[at].additiveRf = config.crashAreaVoltSeconds * config.sampleRate;
  const uint32_t step = static_cast<uint32_t>(config.sampleRate * 0.005f);
  for (uint32_t i = 0; i < frames; i += step) {
    const uint32_t n = std::min(step, frames - i);
    radio.processAmAudio(input.data() + i, output.data() + i, n, config.carrierRmsVolts, .3f,
                         paths.data() + i, terminal.data() + i);
    double peak = 0;
    for (uint32_t k = i; k < i + n; ++k) peak = std::max(peak, double(std::fabs(terminal[k])));
    std::cout << 1000.0 * (i + n) / config.sampleRate << "," << radio.demod.am.avcEnv << ","
              << radio.demod.am.detectorNode << "," << peak << "\n";
  }
  std::cout << "\n";
}

// Playback-chain trace on silence: the full preview pipeline (broadcast
// source, reception, receiver) every 50 ms, to follow slow loops.
void runPreviewTrace(const HarnessConfig& config) {
  std::cout << "[preview_trace]\n";
  std::cout << "time_ms,avc_line_volts,detector_volts,first_audio_grid,first_audio_plate,"
               "terminal_peak,main_bplus,aux_bplus,audio_bplus,screen_scale\n";
  Radio1938 radio;
  radio.applyReceiverProfile(config.receiverProfile);
  if (!config.settingsPath.empty()) {
    std::string error;
    if (!applyRadioSettingsIni(radio, config.settingsPath, config.presetName, &error))
      fail("failed to apply radio settings: " + error);
  }
  radio.init(1, config.sampleRate, config.bandwidthHz, 0.012f);
  RadioAmIngressConfig ingress;
  ingress.reception = radioReceptionConfigForProfile(config.reception);
  if (config.atmosphericNoise) ingress.reception.atmosphericNoiseEnabled = true;
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, RadioPreviewConfig{}, config.sampleRate);
  const uint32_t step = static_cast<uint32_t>(config.sampleRate * 0.05f);
  std::vector<float> block(step);
  const uint32_t blocks = static_cast<uint32_t>(config.traceSeconds / 0.05f);
  if (config.traceDetailMs >= 0) {
    // Per-sample detail over 60 ms from the given time, every fourth sample.
    const uint32_t startFrame =
        static_cast<uint32_t>(config.traceDetailMs * 1e-3f * config.sampleRate);
    const uint32_t endFrame = startFrame + static_cast<uint32_t>(0.06f * config.sampleRate);
    std::vector<float> one(1);
    std::cout << "frame,detector,first_grid,first_plate,driver_grid,interstage_primary,"
                 "out_grid_a,out_grid_b,out_primary_v,out_primary_a,inter_primary_a,main,aux,speaker\n";
    for (uint32_t frame = 0; frame < endFrame; ++frame) {
      one[0] = 0.0f;
      preview.runBlock(radio, one.data(), 1, 1);
      if (frame < startFrame || frame % 4) continue;
      const auto& supply = radio.powerSupply.circuit;
      std::cout << frame << "," << radio.demod.am.detectorNode << ","
                << radio.receiverCircuit.gridVoltage << ","
                << radio.receiverCircuit.tubePlateVoltage << "," << radio.power.gridVoltage
                << "," << radio.power.interstageCt.primaryVoltage << ","
                << radio.power.outputGridAVolts << "," << radio.power.outputGridBVolts << ","
                << radio.power.outputTransformer.primaryVoltage << ","
                << radio.power.outputTransformer.primaryCurrent << ","
                << radio.power.interstageCt.primaryCurrent << ","
                << supply.volts(RectifierSupply::Main) << ","
                << supply.volts(RectifierSupply::Auxiliary) << ","
                << radio.speakerStage.physicalDriveVolts << "\n";
    }
    return;
  }
  for (uint32_t t = 0; t < blocks; ++t) {
    std::fill(block.begin(), block.end(), 0.0f);
    preview.runBlock(radio, block.data(), step, 1);
    double peak = 0;
    for (float v : block) peak = std::max(peak, double(std::fabs(v)));
    const auto& supply = radio.powerSupply.circuit;
    std::cout << 50 * (t + 1) << "," << radio.demod.am.avcEnv << ","
              << radio.demod.am.detectorNode << "," << radio.receiverCircuit.gridVoltage << ","
              << radio.receiverCircuit.tubePlateVoltage << "," << peak << ","
              << supply.volts(RectifierSupply::Main) << ","
              << supply.volts(RectifierSupply::Auxiliary) << ","
              << supply.volts(RectifierSupply::Audio) << "," << radioScreenSupplyScale(radio)
              << "\n";
  }
  std::cout << "\n";
}

// Selective fading snapshots: equal groundwave and skywave (night-skywave
// gains), the skywave 0.3 ms late, its RF phase stepped around the circle.
// Fades are minutes long, so each phase is held while the AVC settles.
void runSelectiveFading(const HarnessConfig& config) {
  std::cout << "[fading]\n";
  std::cout << "sky_phase_deg,carrier_db,terminal_audio_db,thd_db,avc_line_volts\n";
  const auto reception = radioReceptionConfigForProfile(RadioReceptionProfile::NightSkywave);
  const float delaySamples = 0.0003f * config.sampleRate;
  const auto input = makeSine(config.sampleRate, kSteadyTestFrames, 1000.0f, 1.0f);
  const size_t start = static_cast<size_t>(config.sampleRate * 0.25f);
  HarnessConfig levelConfig = config;
  levelConfig.observeTerminalVoltage = true;
  double reference = 0;
  for (int step = 0; step <= 12; ++step) {
    const double phase = 6.283185307179586 * step / 12;
    std::vector<RadioAmReceptionSample> paths(input.size());
    for (auto& path : paths) {
      path.desiredCarrierI = reception.groundwaveGain;
      path.skywaveI = static_cast<float>(reception.skywaveGain * std::cos(phase));
      path.skywaveQ = static_cast<float>(reception.skywaveGain * std::sin(phase));
      path.skywaveDelaySamples = delaySamples;
    }
    Radio1938 radio = makeRadio(levelConfig, 0, true);
    std::vector<float> output(input.size()), terminal(input.size());
    radio.processAmAudio(input.data(), output.data(), static_cast<uint32_t>(input.size()),
                         levelConfig.carrierRmsVolts, .3f, paths.data(), terminal.data());
    std::vector<float> tail(terminal.begin() + start, terminal.end());
    const auto spectrum = computeSpectrumPeakAmplitudes(tail, config.sampleRate);
    const double h1 = peakAmplitudeAtHz(spectrum, config.sampleRate, 1000.0f);
    double harmonics = 0;
    for (float hz : {2000.0f, 3000.0f, 4000.0f, 5000.0f}) {
      const double h = peakAmplitudeAtHz(spectrum, config.sampleRate, hz);
      harmonics += h * h;
    }
    const double audio = measureSignalStats(terminal, start).rms;
    if (step == 0) reference = audio;
    const double carrier = std::hypot(reception.groundwaveGain + reception.skywaveGain * std::cos(phase),
                                      reception.skywaveGain * std::sin(phase));
    std::cout << 30 * step << "," << dbFromRatio(carrier) << ","
              << dbFromRatio(audio / std::max(reference, 1e-20)) << ","
              << dbFromRatio(std::sqrt(harmonics) / std::max(h1, 1e-20)) << ","
              << radio.demod.am.avcEnv << "\n";
  }
  std::cout << "\n";
}

void runSinadSweep(const HarnessConfig& config,
                   std::string_view sectionName,
                   const std::vector<float>& carrierLevels) {
  std::cout << "[" << sectionName << "]\n";
  std::cout << "carrier_rms,tone_rms,residual_rms,sinad_db,sndr_db,max_digital,"
               "speaker_ref_ratio,validation_failed\n";
  auto input =
      makeSine(config.sampleRate, kSteadyTestFrames, 1000.0f,
               kNominalProgramPeak);
  float modulationIndex = nominalHarnessModulationIndex(config);
  float effectiveNoiseWeight =
      std::max(config.noiseWeight, config.noiseOnlyWeight);
  size_t analysisStart = static_cast<size_t>(config.sampleRate * 0.20f);
  for (float carrierRms : carrierLevels) {
    HarnessConfig levelConfig = config;
    levelConfig.carrierRmsVolts = carrierRms;
    RunResult run =
        runAmProgram(levelConfig, input, modulationIndex, effectiveNoiseWeight);
    ToneMetrics metrics =
        measureToneMetrics(run.output, config.sampleRate, 1000.0f, analysisStart);
    std::cout << carrierRms << "," << metrics.signalRms << ","
              << metrics.residualRms << "," << metrics.sinadDb << ","
              << metrics.sndrDb << "," << run.radio.calibration.maxDigitalOutput
              << "," << run.radio.calibration.speakerReferenceRmsRatio << ","
              << (run.radio.calibration.validationFailed ? 1 : 0) << "\n";
  }
  std::cout << "\n";
}

void runSinad(const HarnessConfig& config) {
  const std::vector<float> carrierLevels = {0.1800f, 0.1200f, 0.0800f, 0.0500f,
                                            0.0300f, 0.0200f, 0.0120f, 0.0080f,
                                            0.0050f, 0.0030f};
  runSinadSweep(config, "sinad", carrierLevels);
}

void runWeakSignal(const HarnessConfig& config) {
  const std::vector<float> carrierLevels = {0.0200f, 0.0120f, 0.0080f, 0.0050f,
                                            0.0030f, 0.0020f};
  runSinadSweep(config, "weak", carrierLevels);
}

void runTransient(const HarnessConfig& config) {
  std::cout << "[transient]\n";
  std::cout << "metric,value\n";
  float effectiveNoiseWeight =
      std::max(config.noiseWeight, 0.5f * config.noiseOnlyWeight);
  float carrierHz = selectHarnessCarrierHz(config, effectiveNoiseWeight);
  // Each carrier-on or carrier-off plateau must be long enough for the 75 ms
  // AVC network to cross both 10% and 90%. Four 2 Hz cycles also leave the
  // edge finder the six settling half-periods it deliberately skips.
  constexpr float kBurstHz = 2.0f;
  const uint32_t transientFrames =
      static_cast<uint32_t>(std::ceil(2.0f * config.sampleRate));
  auto rf = makeCarrierBurstRf(config.sampleRate, transientFrames, carrierHz,
                               config.carrierRmsVolts, kBurstHz);
  EnvelopeTraceResult run =
      runRfInputWithTrace(config, rf, effectiveNoiseWeight);
  EdgeMetrics audioMetrics =
      measureEnvelopeMetrics(run.audioEnv, config.sampleRate, kBurstHz);
  EdgeMetrics avcMetrics =
      measureEnvelopeMetrics(run.avcEnv, config.sampleRate, kBurstHz);
  std::cout << "trace,carrier_burst\n";
  std::cout << "audio_rise_10_90_ms," << audioMetrics.rise10To90Ms << "\n";
  std::cout << "audio_fall_90_10_ms," << audioMetrics.fall90To10Ms << "\n";
  std::cout << "audio_overshoot_fraction," << audioMetrics.overshoot << "\n";
  std::cout << "audio_undershoot_fraction," << audioMetrics.undershoot << "\n";
  std::cout << "audio_dc_shift," << audioMetrics.dcShift << "\n";
  std::cout << "avc_rise_10_90_ms," << avcMetrics.rise10To90Ms << "\n";
  std::cout << "avc_fall_90_10_ms," << avcMetrics.fall90To10Ms << "\n";
  std::cout << "avc_overshoot_fraction," << avcMetrics.overshoot << "\n";
  std::cout << "avc_undershoot_fraction," << avcMetrics.undershoot << "\n";
  std::cout << "avc_dc_shift," << avcMetrics.dcShift << "\n\n";
}

bool printReferenceRow(const char* metric,
                       double measured,
                       double low,
                       double high) {
  bool inBand = (measured >= low && measured <= high);
  std::cout << metric << "," << measured << "," << low << "," << high << ","
            << (inBand ? 1 : 0) << "\n";
  return inBand;
}

bool runReference(const HarnessConfig& config) {
  std::cout << "[reference]\n";
  std::cout << "metric,measured,low,high,in_band\n";
  // Bandwidth edges describe the receiver, not the volume-dependent loudness
  // compensation, which is checked separately against the impedance reference.
  HarnessConfig flatConfig = config;
  flatConfig.withoutLoudnessBranch = true;
  flatConfig.levelMatchedFlat = true;
  SweepSummary sweep = summarizeSweep(measureSweepPoints(flatConfig));
  // Keep receiver quality limits independent of the rising period speaker
  // response, and ALSO validate/report the complete acoustic output below.
  HarnessConfig electricalConfig = config;
  electricalConfig.observeTerminalVoltage = true;
  HarnessConfig flatElectricalConfig = electricalConfig;
  flatElectricalConfig.withoutLoudnessBranch = true;
  flatElectricalConfig.levelMatchedFlat = true;
  const auto flatElectricalPoints = measureSweepPoints(flatElectricalConfig);
  SweepSummary electricalSweep = summarizeSweep(flatElectricalPoints);
  double loudnessErrorDb = 0.0;
  {
    const Radio1938 probe = makeRadio(config, 0.0f, false);
    if (probe.receiverCircuit.volumeControlTapResistanceOhms > 0.0f) {
      // A linear-network check: at 10% modulation the detector stays in its
      // linear region, so the branch alone separates the two sweeps.
      HarnessConfig linearConfig = electricalConfig;
      linearConfig.modulationIndex = 0.1f;
      HarnessConfig linearFlatConfig = linearConfig;
      linearFlatConfig.withoutLoudnessBranch = true;
      const auto fullPoints = measureSweepPoints(linearConfig);
      const auto linearFlatPoints = measureSweepPoints(linearFlatConfig);
      for (size_t i = 0; i < fullPoints.size(); ++i) {
        const double hz = fullPoints[i].frequencyHz;
        if (hz < 100.0 || hz > 4000.0) continue;
        const std::complex<double> laplace{0, 2 * 3.14159265358979323846 * hz};
        const double predicted = dbFromRatio(
            std::abs(receiverInputReference(probe.receiverCircuit, laplace).grid) /
            std::abs(receiverInputReference(probe.receiverCircuit, laplace, false).grid));
        const double measured = fullPoints[i].gainDb - linearFlatPoints[i].gainDb;
        loudnessErrorDb = std::max(loudnessErrorDb, std::abs(measured - predicted));
      }
    }
  }
  ReferenceAnchorSummary electricalAnchors =
      measureReferenceAnchorSummary(electricalConfig);
  double electricalImdDb = measureImdSummaryDb(electricalConfig);
  EdgeMetrics envelope =
      measureEnvelopeMetrics(
          runAmProgramWithEnvelopeTrace(
              config, makeSquare(config.sampleRate, kEnvelopeTestFrames, 30.0f, 0.85f),
              config.modulationIndex, config.noiseWeight)
              .audioEnv,
          config.sampleRate, 30.0f);
  ReferenceAnchorSummary anchors = measureReferenceAnchorSummary(config);
  double imdDb = measureImdSummaryDb(config);
  // Run a nominal program to populate calibration/diagnostic metrics used
  // for reference checks.
  auto nominalInput =
      makeSine(config.sampleRate, kSteadyTestFrames, 1000.0f,
               kNominalProgramPeak);
  HarnessConfig nominalConfig = config;
  nominalConfig.carrierRmsVolts = kDefaultCarrierRmsVolts;
  RunResult nominalRun = runAmProgram(nominalConfig, nominalInput,
                   nominalHarnessModulationIndex(config),
                   std::max(config.noiseWeight, config.noiseOnlyWeight));
  Radio1938& radio = nominalRun.radio;
  const ReceiverReferenceBands& bands =
      referenceBandsForProfile(radio.receiverProfile);
  double detectorTauUs = 1e6 * effectiveDetectorAudioTauSeconds(radio);
  double avcTauMs = 1e3 * avcTauSeconds(radio);

  // Working bands only. The frequency-domain limits are still coarse. The
  // detector / AVC timing rows below are based on named RC state and service
  // sideband constraints, not on arbitrary output-envelope taste metrics.
  bool allInBand = true;
  allInBand &= printReferenceRow("terminal_low_3db_hz", electricalSweep.low3dBHz,
                                 bands.terminalLow3dBMin, bands.terminalLow3dBMax);
  allInBand &= printReferenceRow("terminal_high_3db_hz", electricalSweep.high3dBHz,
                                 bands.terminalHigh3dBMin, bands.terminalHigh3dBMax);
  // The ideal-source reference omits the detector's own source resistance;
  // 0.3 dB covers that.
  allInBand &= printReferenceRow("loudness_branch_error_db", loudnessErrorDb, 0.0, 0.3);
  allInBand &= printReferenceRow("terminal_imd_db", electricalImdDb,
                                 -100.0, bands.terminalImdDbMax);
  allInBand &= printReferenceRow("terminal_sinad_nominal_db",
                                 electricalAnchors.sinadNominalDb,
                                 bands.terminalSinadDbMin, 100.0);
  // These edges bound the TOP of the broad 2-3 kHz rise, not usable bass.
  // Source-labelled acoustic tests constrain bass and relative levels too.
  allInBand &= printReferenceRow("acoustic_peak_low_3db_hz", sweep.low3dBHz,
                                 1000.0f, 2500.0f);
  allInBand &= printReferenceRow("acoustic_peak_high_3db_hz", sweep.high3dBHz,
                                 bands.acousticPeakHigh3dBMin,
                                 bands.acousticPeakHigh3dBMax);
  // Acoustic weighting raises existing harmonics and noise around 2-3 kHz.
  // These are engineering guards, not historical distortion measurements.
  allInBand &= printReferenceRow("acoustic_imd_db", imdDb, -100.0,
                                 bands.acousticImdDbMax);
  allInBand &= printReferenceRow("detector_tau_us", detectorTauUs,
                                 config.receiverProfile == RadioReceiverProfile::Philco37116 ? kPhilcoDetectorTauUsMin : kTypicalDetectorTauUsMin,
                                 config.receiverProfile == RadioReceiverProfile::Philco37116 ? kPhilcoDetectorTauUsMax : kTypicalDetectorTauUsMax);
  const bool philco = config.receiverProfile == RadioReceiverProfile::Philco37116;
  allInBand &= printReferenceRow("avc_tau_ms", avcTauMs,
                                 philco ? kPhilcoAvcTauMsMin : kTypicalAvcTauMsMin,
                                 philco ? kPhilcoAvcTauMsMax : kTypicalAvcTauMsMax);
  allInBand &= printReferenceRow(
      "avc_to_detector_tau_ratio",
      avcTauMs / std::max(detectorTauUs * 1e-3, 1e-9), 50.0, 1e9);
  allInBand &= printReferenceRow("envelope_rise_ms_observed",
                                 envelope.rise10To90Ms, 0.0, 10.0);
  allInBand &= printReferenceRow("envelope_fall_ms_observed",
                                 envelope.fall90To10Ms, 0.0, 10.0);
  allInBand &= printReferenceRow("acoustic_sinad_nominal_db", anchors.sinadNominalDb,
                                 bands.acousticSinadDbMin, 100.0);
  // Speaker/reference and digital output targets (measured from nominal run)
  allInBand &= printReferenceRow(
      "speaker_reference_ratio", anchors.speakerReferenceRatio,
      bands.speakerReferenceRatioMin, bands.speakerReferenceRatioMax);
  allInBand &= printReferenceRow("max_digital_output", anchors.maxDigitalOutput,
                                 kTargetMaxDigitalOutputMin,
                                 kTargetMaxDigitalOutputMax);
  // Diagnostic flags expected to be zero in nominal bench
  allInBand &= printReferenceRow(
      "power_clip", nominalRun.radio.diagnostics.powerClip ? 1 : 0,
      kTargetFlagZero, kTargetFlagZero);
  allInBand &= printReferenceRow(
      "speaker_clip", nominalRun.radio.diagnostics.speakerClip ? 1 : 0,
      kTargetFlagZero, kTargetFlagZero);
  allInBand &= printReferenceRow(
      "output_clip", nominalRun.radio.diagnostics.outputClip ? 1 : 0,
      kTargetFlagZero, kTargetFlagZero);
  // Programme loudness matches the unfiltered path, so a steady 85% tone is
  // limited by design; reported only. The physical clip flags above stay zero.
  printReferenceRow("final_limiter_active",
                    nominalRun.radio.diagnostics.finalLimiterActive ? 1 : 0, 0, 1);
  // The AVC loop must not reach into the audio band. With the Philco's
  // former 75 ms prior the loop closed near 50 Hz and chased bass
  // modulation: -22 dBc of second harmonic at the terminals for an 80 Hz
  // tone at 50% modulation, independent of the volume control.
  {
    HarnessConfig toneConfig = config;
    toneConfig.observeTerminalVoltage = true;
    const auto tone = makeSine(config.sampleRate, kSteadyTestFrames, 80.0f,
                               kOverdriveProgramPeak);
    RunResult run = runAmProgram(toneConfig, tone, 0.5f, config.noiseWeight);
    // Exact DFT over the last 0.25 s: whole periods of 80, 120 and 160 Hz,
    // so supply hum at 120 Hz cannot leak into the harmonic.
    const auto amplitudeAt = [&](double hz) {
      const size_t n = static_cast<size_t>(0.25 * config.sampleRate);
      const size_t start = run.output.size() - n;
      std::complex<double> sum{};
      for (size_t i = 0; i < n; ++i) {
        const double phase = kRadioTwoPi * hz * double(i) / config.sampleRate;
        sum += double(run.output[start + i]) * std::complex<double>(std::cos(phase), -std::sin(phase));
      }
      return 2.0 * std::abs(sum) / double(n);
    };
    const double h1 = amplitudeAt(80.0);
    const double h2 = amplitudeAt(160.0);
    allInBand &= printReferenceRow("terminal_80hz_h2_dbc",
                                   dbFromRatio(h2 / std::max(h1, 1e-20)), -100.0,
                                   philco ? kPhilcoTerminal80HzH2DbcMax
                                          : kTypicalTerminal80HzH2DbcMax);
  }
  // IF center and nominal output power anchor
  allInBand &= printReferenceRow("if_center_hz", radio.ifStrip.ifCenterHz,
                                 kTargetIfCenterMinHz,
                                 kTargetIfCenterMaxHz);
  allInBand &= printReferenceRow(
      "nominal_output_power_watts", radio.power.nominalOutputPowerWatts,
      bands.nominalOutputPowerMinWatts, bands.nominalOutputPowerMaxWatts);
  std::cout << "\n";
  return allInBand;
}

void runNoise(const HarnessConfig& config) {
  std::cout << "[noise]\n";
  std::cout << "metric,value\n";
  auto zeros = makeSilence(kSteadyTestFrames);
  RunResult run = runRfInput(config, zeros, config.noiseOnlyWeight);
  SignalStats stats = measureSignalStats(run.output);
  std::vector<double> spectrum =
      computeSpectrumPeakAmplitudes(run.output, config.sampleRate);
  std::cout << "rms," << stats.rms << "\n";
  std::cout << "peak," << stats.peak << "\n";
  std::cout << "hum_50_120_rms," << bandRmsFromSpectrum(spectrum, config.sampleRate, 50.0f, 120.0f) << "\n";
  std::cout << "low_120_400_rms," << bandRmsFromSpectrum(spectrum, config.sampleRate, 120.0f, 400.0f) << "\n";
  std::cout << "mid_400_1500_rms," << bandRmsFromSpectrum(spectrum, config.sampleRate, 400.0f, 1500.0f) << "\n";
  std::cout << "hiss_1500_5000_rms," << bandRmsFromSpectrum(spectrum, config.sampleRate, 1500.0f, 5000.0f) << "\n\n";
}

void runAmValidate(const HarnessConfig& config) {
  std::cout << "[am_validate]\n";
  std::cout << "metric,value\n";
  AmValidationSummary summary = measureAmValidationSummary(config);
  bool compareSkipped =
      std::max(config.noiseWeight, config.noiseOnlyWeight) > 0.0f;
  if (compareSkipped) {
    std::cout << "am_output_compare_skipped,1\n";
  }
  std::cout << "am_output_max_abs_diff," << summary.maxOutputAbsDiff << "\n";
  std::cout << "carrier_rms," << summary.carrierRms << "\n";
  std::cout << "sideband_upper_rms," << summary.sideUpperRms << "\n";
  std::cout << "sideband_lower_rms," << summary.sideLowerRms << "\n";
  std::cout << "sideband_expected_per_side_rms," << summary.expectedSideRms
            << "\n";
  std::cout << "sideband_avg_ratio_vs_expected," << summary.sidebandRatio
            << "\n";
  std::cout << "sideband_ratio_in_band,"
            << (summary.sidebandRatioInBand ? 1 : 0) << "\n";
  std::cout << "detector_dc," << summary.detectorDc << "\n";
  std::cout << "detector_tone_1khz," << summary.detectorTone << "\n";
  std::cout << "detector_tone_to_dc_ratio,"
            << summary.detectorToneToDcRatio << "\n";
  std::cout << "mod_index_config," << summary.modulationIndexConfig << "\n";
  std::cout << "mod_index_expected_effective,"
            << summary.effectiveModulationDepth << "\n";
  std::cout << "mod_index_measured_env,"
            << summary.modulationIndexMeasuredEnv << "\n";
  std::cout << "mod_index_in_band," << (summary.modulationIndexInBand ? 1 : 0)
            << "\n";
  std::cout << "detector_sinad_db," << summary.detectorSinadDb << "\n";
  std::cout << "output_sinad_db," << summary.outputSinadDb << "\n";
  std::cout << "speaker_reference_ratio," << summary.speakerReferenceRatio
            << "\n";
  std::cout << "max_digital_output," << summary.maxDigitalOutput << "\n";
  std::cout << "if_center_hz," << summary.ifCenterHz << "\n";
  std::cout << "nominal_output_power_watts,"
            << summary.nominalOutputPowerWatts << "\n";
  std::cout << "waveform_substeps," << summary.waveformSubsteps << "\n\n";

  if (!compareSkipped && summary.maxOutputAbsDiff > 1e-4) {
    fail("AM output mismatch between processAmAudio and envelope trace: " +
         std::to_string(summary.maxOutputAbsDiff));
  }
  if (!summary.sidebandRatioInBand) {
    fail("AM sideband ratio out of range: measured " +
         std::to_string(summary.sidebandRatio) + ", expected [" +
         std::to_string(kAmSidebandRatioMin) + ", " +
         std::to_string(kAmSidebandRatioMax) + "]");
  }
  if (!summary.modulationIndexInBand) {
    fail("AM modulation index out of range: measured " +
         std::to_string(summary.modulationIndexMeasuredEnv) +
         ", expected around " +
         std::to_string(summary.effectiveModulationDepth));
  }
}

void runAmConvergence(const HarnessConfig& config) {
  std::cout << "[am_convergence]\n";
  std::cout << "samples_per_cycle,waveform_substeps,elapsed_ms,"
               "sideband_avg_ratio_vs_expected,mod_index_measured_env,"
               "detector_sinad_db,output_sinad_db,sinad_nominal_db,"
               "speaker_reference_ratio,max_digital_output\n";
  constexpr std::array<float, 5> kSamplesPerCycle = {2.0f, 3.0f, 4.0f, 6.0f,
                                                      8.0f};
  for (float samplesPerCycle : kSamplesPerCycle) {
    HarnessConfig stepConfig = config;
    stepConfig.detectorWaveformSamplesPerCycle = samplesPerCycle;
    auto startTime = std::chrono::steady_clock::now();
    AmValidationSummary amSummary = measureAmValidationSummary(stepConfig);
    ReferenceAnchorSummary referenceSummary =
        measureReferenceAnchorSummary(stepConfig);
    auto endTime = std::chrono::steady_clock::now();
    double elapsedMs = std::chrono::duration<double, std::milli>(
                           endTime - startTime)
                           .count();
    std::cout << samplesPerCycle << "," << amSummary.waveformSubsteps << ","
              << elapsedMs << "," << amSummary.sidebandRatio << ","
              << amSummary.modulationIndexMeasuredEnv << ","
              << amSummary.detectorSinadDb << "," << amSummary.outputSinadDb
              << "," << referenceSummary.sinadNominalDb << ","
              << referenceSummary.speakerReferenceRatio << ","
              << referenceSummary.maxDigitalOutput << "\n";
  }
  std::cout << "\n";
}

void runPowerBench(const HarnessConfig& config);

// ---------------------------------------------------------------------------
// Whole-output programme test.
//
// The other sections test one stage at a time. This one runs the complete
// playback path (broadcast source, reception, receiver, room, makeup and
// limiter) in stereo on a deterministic synthetic programme and compares it
// with the unfiltered source it replaces: loudness, the share of each band
// that is a linear image of the source, peaks and limiter load. Stage tests
// passed while the Philco played at a third of its rated power and its AVC
// loop chased the bass; this test fails on both.

class ProgrammeRng {
 public:
  explicit ProgrammeRng(uint64_t seed) : state_(seed) {}
  double uniform() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<double>(state_ >> 11) * (1.0 / 9007199254740992.0);
  }
  double gauss() {
    const double u1 = std::max(uniform(), 1e-12);
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(kRadioTwoPi * uniform());
  }

 private:
  uint64_t state_;
};

// Direct-form biquad with RBJ band-pass coefficients, local to the generator.
struct ProgrammeBiquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  void bandpass(double fs, double hz, double q) {
    const double w = kRadioTwoPi * hz / fs, alpha = std::sin(w) / (2 * q);
    const double a0 = 1 + alpha;
    b0 = alpha / a0;
    b1 = 0;
    b2 = -alpha / a0;
    a1 = -2 * std::cos(w) / a0;
    a2 = (1 - alpha) / a0;
  }
  double process(double x) {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

struct StereoProgramme {
  std::vector<float> left, right;
  size_t musicStart = 0, musicEnd = 0, speechStart = 0, speechEnd = 0;
};

constexpr double kProgrammeLeadSeconds = 1.5;
constexpr double kProgrammeMusicSeconds = 12.0;
constexpr double kProgrammeGapSeconds = 0.5;
constexpr double kProgrammeSpeechSeconds = 8.0;

void addPanned(StereoProgramme& p, size_t i, double x, double pan) {
  const double angle = (pan + 1.0) * kRadioPi / 4.0;
  p.left[i] += static_cast<float>(x * std::cos(angle) * std::sqrt(2.0));
  p.right[i] += static_cast<float>(x * std::sin(angle) * std::sqrt(2.0));
}

// Twelve seconds of music: a four-chord progression of decaying piano-like
// voices across the stereo field, a melody line, a bass line down to 55 Hz,
// kick, snare and hi-hat, swelling from -9 dB to full level and back. Then eight seconds of
// speech-like sound: voiced syllables at 100-150 Hz through vowel formants,
// with unvoiced onsets, in syllable and phrase rhythm.
StereoProgramme makeProgramme(double fs) {
  StereoProgramme p;
  const auto at = [fs](double seconds) { return static_cast<size_t>(seconds * fs); };
  p.musicStart = at(kProgrammeLeadSeconds);
  p.musicEnd = p.musicStart + at(kProgrammeMusicSeconds);
  p.speechStart = p.musicEnd + at(kProgrammeGapSeconds);
  p.speechEnd = p.speechStart + at(kProgrammeSpeechSeconds);
  p.left.assign(p.speechEnd + at(0.5), 0.0f);
  p.right.assign(p.left.size(), 0.0f);
  ProgrammeRng rng(0x5ADD1938u);

  const std::array<std::array<double, 3>, 4> chords{{{261.63, 329.63, 392.00},
                                                     {220.00, 261.63, 329.63},
                                                     {174.61, 220.00, 261.63},
                                                     {196.00, 246.94, 293.66}}};
  const std::array<double, 4> bass{65.41, 55.00, 87.31, 98.00};
  const std::array<double, 3> voicePan{-0.6, 0.0, 0.6};
  ProgrammeBiquad snareBand;
  snareBand.bandpass(fs, 1800.0, 0.8);
  double previousNoise = 0.0;
  const std::array<double, 6> melodyNotes{523.25, 587.33, 659.25, 783.99, 880.00, 1046.50};
  // Levels give the long-term spectrum of the ten real tracks used for the
  // loudness match: 23 / 29 / 44 / 3.2 / 0.7% of the power below 150 Hz,
  // 150-500 Hz, 0.5-2 kHz, 2-5 kHz and above (tracks: 24 / 29 / 44 / 3.4 /
  // 0.5%).
  constexpr double aVoice = 0.22, aBass = 0.30, aKick = 0.50, aSnare = 0.25, aHat = 0.05,
                   aMelody = 0.30;
  double melodyPhase = 0.0;
  size_t melodyNote = 0, lastMelodyStep = SIZE_MAX;
  for (size_t i = p.musicStart; i < p.musicEnd; ++i) {
    const double t = static_cast<double>(i - p.musicStart) / fs;
    const double swell = std::sin(kRadioPi * t / kProgrammeMusicSeconds);
    const double gain = std::pow(10.0, (-9.0 + 9.0 * swell * swell) / 20.0);
    const size_t bar = static_cast<size_t>(t / 2.0) % chords.size();
    const double inBar = std::fmod(t, 2.0);
    for (size_t v = 0; v < 3; ++v) {
      // Struck at the bar and again half way, slightly arpeggiated.
      const double onset = 0.12 * static_cast<double>(v);
      double since = inBar - onset;
      if (since >= 1.0) since -= 1.0;
      if (since < 0.0) continue;
      const double f = chords[bar][v];
      double tone = 0.0;
      for (int n = 1; n <= 12 && n * f < 9000.0; ++n) {
        tone += std::pow(n, -1.3) * std::exp(-since * (1.2 + 0.4 * n)) *
                std::sin(kRadioTwoPi * n * f * since);
      }
      addPanned(p, i, aVoice * gain * tone, voicePan[v]);
    }
    const double fb = bass[bar];
    double bassTone = 0.0;
    for (int n = 1; n <= 4; ++n)
      bassTone += std::sin(kRadioTwoPi * n * fb * inBar) / n;
    bassTone *= std::min(1.0, inBar / 0.02) * std::exp(-0.8 * inBar);
    addPanned(p, i, aBass * gain * bassTone, 0.0);
    // A sustained melody line over the chords, eighth notes with vibrato.
    const size_t melodyStep = static_cast<size_t>(t / 0.25);
    if (melodyStep != lastMelodyStep) {
      lastMelodyStep = melodyStep;
      melodyNote = static_cast<size_t>(rng.uniform() * melodyNotes.size()) % melodyNotes.size();
    }
    const double inNote = t - 0.25 * static_cast<double>(melodyStep);
    const double fm = melodyNotes[melodyNote] * (1.0 + 0.005 * std::sin(kRadioTwoPi * 5.0 * t));
    melodyPhase += fm / fs;
    melodyPhase -= std::floor(melodyPhase);
    double lead = 0.0;
    for (int n = 1; n <= 6; ++n) lead += std::pow(n, -1.5) * std::sin(kRadioTwoPi * n * melodyPhase);
    lead *= std::min({1.0, inNote / 0.02, (0.25 - inNote) / 0.03});
    addPanned(p, i, aMelody * gain * lead, 0.3);

    const double beat = std::fmod(t, 1.0);
    const double kickPhase = kRadioTwoPi * (50.0 * beat + 70.0 * 0.05 * (1.0 - std::exp(-beat / 0.05)));
    addPanned(p, i, aKick * gain * std::exp(-beat / 0.15) * std::sin(kickPhase), 0.0);

    const double noise = rng.gauss();
    const double snareSince = std::fmod(t + 0.5, 1.0);
    const double snare = snareBand.process(noise) * 3.0 +
                         0.5 * std::sin(kRadioTwoPi * 180.0 * snareSince);
    addPanned(p, i, aSnare * gain * std::exp(-snareSince / 0.08) * snare, -0.2);
    const double hatSince = std::fmod(t, 0.25);
    addPanned(p, i, aHat * gain * std::exp(-hatSince / 0.025) * (noise - previousNoise), 0.5);
    previousNoise = noise;
  }

  const std::array<std::array<double, 3>, 5> vowels{{{730, 1090, 2440},
                                                     {270, 2290, 3010},
                                                     {530, 1840, 2480},
                                                     {570, 840, 2410},
                                                     {300, 870, 2240}}};
  std::vector<float> speech(p.speechEnd - p.speechStart, 0.0f);
  size_t cursor = 0;
  int syllable = 0;
  double phase = 0.0;
  while (cursor < speech.size()) {
    const size_t length = at(0.12 + 0.18 * rng.uniform());
    const auto& vowel = vowels[static_cast<size_t>(rng.uniform() * vowels.size()) % vowels.size()];
    const double f0 = 100.0 + 50.0 * rng.uniform();
    std::array<ProgrammeBiquad, 3> formant;
    for (size_t k = 0; k < 3; ++k) formant[k].bandpass(fs, vowel[k], 6.0);
    for (size_t k = 0; k < length && cursor + k < speech.size(); ++k) {
      const double s = static_cast<double>(k) / fs;
      const double total = static_cast<double>(length) / fs;
      const double envelope = std::min({1.0, s / 0.03, (total - s) / 0.03});
      const double f = f0 * (1.0 + 0.08 * std::sin(kRadioPi * s / total));
      phase += f / fs;
      phase -= std::floor(phase);
      double source = 0.0;
      for (int n = 1; n * f < 4000.0; ++n) source += std::sin(kRadioTwoPi * n * phase) / n;
      const double voiced = formant[0].process(source) + 0.5 * formant[1].process(source) +
                            0.25 * formant[2].process(source);
      const double unvoiced = s < 0.04 ? 0.3 * rng.gauss() * (1.0 - s / 0.04) : 0.0;
      speech[cursor + k] = static_cast<float>(envelope * (voiced + unvoiced));
    }
    cursor += length + at(0.03 + 0.09 * rng.uniform());
    if (++syllable % 5 == 0) cursor += at(0.35);
  }

  double musicPeak = 0.0, speechPeak = 0.0;
  for (size_t i = p.musicStart; i < p.musicEnd; ++i)
    musicPeak = std::max({musicPeak, double(std::fabs(p.left[i])), double(std::fabs(p.right[i]))});
  for (float s : speech) speechPeak = std::max(speechPeak, double(std::fabs(s)));
  const float musicScale = static_cast<float>(0.89 / std::max(musicPeak, 1e-9));
  for (size_t i = p.musicStart; i < p.musicEnd; ++i) {
    p.left[i] *= musicScale;
    p.right[i] *= musicScale;
  }
  const float speechScale = static_cast<float>(0.7 / std::max(speechPeak, 1e-9));
  for (size_t k = 0; k < speech.size(); ++k) {
    p.left[p.speechStart + k] = speech[k] * speechScale;
    p.right[p.speechStart + k] = speech[k] * speechScale;
  }
  return p;
}

// EBU R128 / ITU-R BS.1770-4 integrated loudness of a stereo segment: the
// K-weighting pre-filter and RLB high-pass, 400 ms blocks at 75% overlap,
// the -70 LUFS absolute and -10 LU relative gates.
double integratedLoudnessLufs(const std::vector<float>& left,
                              const std::vector<float>& right, size_t begin,
                              size_t end, double fs) {
  struct Stage {
    double b0, b1, b2, a1, a2;
  };
  double k = std::tan(kRadioPi * 1681.974450955533 / fs);
  const double vh = std::pow(10.0, 3.999843853973347 / 20.0);
  const double vb = std::pow(vh, 0.4996667741545416);
  double q = 0.7071752369554196;
  double a0 = 1.0 + k / q + k * k;
  const Stage shelf{(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0,
                    (vh - vb * k / q + k * k) / a0, 2.0 * (k * k - 1.0) / a0,
                    (1.0 - k / q + k * k) / a0};
  k = std::tan(kRadioPi * 38.13547087602444 / fs);
  q = 0.5003270373238773;
  a0 = 1.0 + k / q + k * k;
  const Stage highpass{1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
  const auto weigh = [&](const std::vector<float>& x) {
    std::vector<double> y(end - begin);
    double s1[2] = {0, 0}, s2[2] = {0, 0};
    for (size_t i = begin; i < end; ++i) {
      double v = x[i];
      const double w1 = shelf.b0 * v + s1[0];
      s1[0] = shelf.b1 * v - shelf.a1 * w1 + s1[1];
      s1[1] = shelf.b2 * v - shelf.a2 * w1;
      const double w2 = highpass.b0 * w1 + s2[0];
      s2[0] = highpass.b1 * w1 - highpass.a1 * w2 + s2[1];
      s2[1] = highpass.b2 * w1 - highpass.a2 * w2;
      y[i - begin] = w2;
    }
    return y;
  };
  const std::vector<double> l = weigh(left), r = weigh(right);
  const size_t block = static_cast<size_t>(0.4 * fs), hop = block / 4;
  std::vector<double> power;
  for (size_t s = 0; s + block <= l.size(); s += hop) {
    double sum = 0.0;
    for (size_t i = s; i < s + block; ++i) sum += l[i] * l[i] + r[i] * r[i];
    power.push_back(sum / static_cast<double>(block));
  }
  const auto loudness = [](double z) { return -0.691 + 10.0 * std::log10(std::max(z, 1e-30)); };
  const auto gatedMean = [&](double thresholdLufs) {
    double sum = 0.0;
    size_t count = 0;
    for (double z : power) {
      if (loudness(z) > thresholdLufs) {
        sum += z;
        ++count;
      }
    }
    return count ? sum / static_cast<double>(count) : 0.0;
  };
  const double relativeGate = loudness(gatedMean(-70.0)) - 10.0;
  return loudness(gatedMean(std::max(relativeGate, -70.0)));
}

// Delay of y against x, from the 300-3000 Hz cross-correlation of their mono
// sums over one window, searched over 0..100 ms (the receivers only delay).
// The band limit keeps the periodic bass line from making the peak ambiguous.
size_t programmeDelay(const std::vector<float>& x, const std::vector<float>& y, size_t start,
                      double fs) {
  constexpr size_t kWindow = 1u << 17;
  std::vector<std::complex<double>> a(kWindow * 2), b(kWindow * 2);
  for (size_t i = 0; i < kWindow && start + i < x.size(); ++i) {
    a[i] = x[start + i];
    b[i] = y[start + i];
  }
  fftInPlace(a);
  fftInPlace(b);
  for (size_t i = 0; i < a.size(); ++i) {
    const size_t bin = std::min(i, a.size() - i);
    const double hz = double(bin) * fs / double(a.size());
    a[i] = hz >= 300.0 && hz <= 3000.0 ? std::conj(b[i] * std::conj(a[i]))
                                       : std::complex<double>{};
  }
  fftInPlace(a);  // conj(FFT(conj(.))) is the inverse up to scale
  size_t best = 0;
  double bestValue = -1.0;
  for (size_t lag = 0; lag < 4800; ++lag) {
    const double v = std::abs(a[lag]);
    if (v > bestValue) {
      bestValue = v;
      best = lag;
    }
  }
  return best;
}

struct ProgrammeSpectra {
  std::vector<double> sxx, syy;
  std::vector<std::complex<double>> sxy;
};

constexpr size_t kProgrammeFft = 8192;

ProgrammeSpectra programmeSpectra(const std::vector<float>& x, const std::vector<float>& y,
                                  size_t begin, size_t end, size_t delay) {
  ProgrammeSpectra s{std::vector<double>(kProgrammeFft / 2 + 1, 0.0),
                     std::vector<double>(kProgrammeFft / 2 + 1, 0.0),
                     std::vector<std::complex<double>>(kProgrammeFft / 2 + 1)};
  std::vector<std::complex<double>> fx(kProgrammeFft), fy(kProgrammeFft);
  for (size_t at = begin; at + kProgrammeFft <= end && at + delay + kProgrammeFft <= y.size();
       at += kProgrammeFft / 2) {
    for (size_t i = 0; i < kProgrammeFft; ++i) {
      const double w = 0.5 - 0.5 * std::cos(kRadioTwoPi * double(i) / double(kProgrammeFft));
      fx[i] = w * x[at + i];
      fy[i] = w * y[at + delay + i];
    }
    fftInPlace(fx);
    fftInPlace(fy);
    for (size_t i = 0; i <= kProgrammeFft / 2; ++i) {
      s.sxx[i] += std::norm(fx[i]);
      s.syy[i] += std::norm(fy[i]);
      s.sxy[i] += fy[i] * std::conj(fx[i]);
    }
  }
  return s;
}

// Linear-to-residual ratio in a band: the power-weighted coherence between
// source and output, as dB of linear image over distortion, noise and
// time-varying gain.
double programmeLinearDb(const ProgrammeSpectra& s, double fs, double lowHz, double highHz) {
  double linear = 0.0, total = 0.0;
  for (size_t i = 1; i < s.sxx.size(); ++i) {
    const double hz = double(i) * fs / double(kProgrammeFft);
    if (hz < lowHz || hz >= highHz || s.sxx[i] <= 0.0) continue;
    linear += std::norm(s.sxy[i]) / s.sxx[i];
    total += s.syy[i];
  }
  const double coherence = std::clamp(linear / std::max(total, 1e-30), 0.0, 1.0 - 1e-9);
  return 10.0 * std::log10(std::max(coherence, 1e-12) / (1.0 - coherence));
}

struct ProgrammeBands {
  double loudnessMusicDb, loudnessSpeechDb, bassLinearDb, midLinearDb, presenceLinearDb;
};

// Bounds. Loudness: one system volume must serve both paths; real tracks
// spread -3.8..+2.3 dB around a 0 dB median, so +/-3 dB on this programme.
// Linear-to-residual floors (dB, 100-250 / 400-1000 / 1000-3000 Hz) are
// regression bounds 4-5 dB below the receivers on this programme, which the
// studio operator rides to the limiter: Philco 13.2 / 18.0 / 19.1, 38-12
// 19.8 / 19.3 / 19.1. The Philco at its former 0.084 knob (4 W on the
// reference tone) measures 4.9 / 7.9 / 8.1. The ratio is taken against the
// ridden programme, so slow riding does not count as distortion.
constexpr ProgrammeBands kPhilcoProgrammeFloor{3.0, 3.0, 9.0, 13.0, 14.0};
constexpr ProgrammeBands kTypicalProgrammeFloor{3.0, 3.0, 15.0, 15.0, 14.0};

bool runProgramme(const HarnessConfig& config) {
  std::cout << "[programme]\n";
  const double fs = config.sampleRate;
  const StereoProgramme programme = makeProgramme(fs);

  Radio1938 radio;
  radio.applyReceiverProfile(config.receiverProfile);
  radio.init(1, config.sampleRate, static_cast<float>(kDefaultRadioBandwidthHz),
             static_cast<float>(kDefaultRadioNoiseAmount));
  if (!config.settingsPath.empty()) {
    std::string error;
    if (!applyRadioSettingsIni(radio, config.settingsPath, config.presetName, &error))
      fail("failed to apply radio settings: " + error);
  }
  RadioAmIngressConfig ingress;
  ingress.reception = radioReceptionConfigForProfile(config.reception);
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, RadioPreviewConfig{}, config.sampleRate);

  const size_t frames = programme.left.size();
  std::vector<float> interleaved(frames * 2);
  for (size_t i = 0; i < frames; ++i) {
    interleaved[2 * i] = programme.left[i];
    interleaved[2 * i + 1] = programme.right[i];
  }
  // The operator's gain, sampled per block: the receivers are compared with
  // what the studio sent, so slow riding does not count as distortion.
  constexpr size_t kBlock = 1024;
  std::vector<float> ridingGain(frames, 1.0f);
  float previousGain = preview.levelRidingGain;
  for (size_t at = 0; at < frames; at += kBlock) {
    const auto n = static_cast<uint32_t>(std::min(kBlock, frames - at));
    preview.runBlock(radio, interleaved.data() + 2 * at, n, 2);
    for (uint32_t k = 0; k < n; ++k)
      ridingGain[at + k] =
          previousGain + (preview.levelRidingGain - previousGain) * float(k + 1) / float(n);
    previousGain = preview.levelRidingGain;
  }
  std::vector<float> outLeft(frames), outRight(frames), inMono(frames), outMono(frames);
  double peak = 0.0;
  for (size_t i = 0; i < frames; ++i) {
    outLeft[i] = interleaved[2 * i];
    outRight[i] = interleaved[2 * i + 1];
    inMono[i] = 0.5f * (programme.left[i] + programme.right[i]) * ridingGain[i];
    outMono[i] = 0.5f * (outLeft[i] + outRight[i]);
    peak = std::max({peak, double(std::fabs(outLeft[i])), double(std::fabs(outRight[i]))});
  }

  const size_t delay = programmeDelay(inMono, outMono, programme.musicStart, fs);
  const auto loudnessDifference = [&](size_t begin, size_t end) {
    return integratedLoudnessLufs(outLeft, outRight, begin + delay,
                                  std::min(end + delay, frames), fs) -
           integratedLoudnessLufs(programme.left, programme.right, begin, end, fs);
  };
  const ProgrammeSpectra music =
      programmeSpectra(inMono, outMono, programme.musicStart, programme.musicEnd, delay);
  const ProgrammeBands measured{
      loudnessDifference(programme.musicStart, programme.musicEnd),
      loudnessDifference(programme.speechStart, programme.speechEnd),
      programmeLinearDb(music, fs, 100.0, 250.0), programmeLinearDb(music, fs, 400.0, 1000.0),
      programmeLinearDb(music, fs, 1000.0, 3000.0)};
  const bool philco = config.receiverProfile == RadioReceiverProfile::Philco37116;
  const ProgrammeBands& floor = philco ? kPhilcoProgrammeFloor : kTypicalProgrammeFloor;
  const auto& d = radio.diagnostics;
  const double limiterAverageDb =
      d.processedSamples ? d.finalLimiterGainReductionDbSum / d.processedSamples : 0.0;

  {
    double total = 0.0;
    std::array<double, 5> share{};
    const std::array<double, 6> edges{0, 150, 500, 2000, 5000, 24000};
    for (size_t i = 1; i < music.sxx.size(); ++i) {
      const double hz = double(i) * fs / double(kProgrammeFft);
      total += music.sxx[i];
      for (size_t b = 0; b < share.size(); ++b)
        if (hz >= edges[b] && hz < edges[b + 1]) share[b] += music.sxx[i];
    }
    std::cout << "source_music_share_pct";
    for (double v : share) std::cout << "," << 100.0 * v / std::max(total, 1e-30);
    std::cout << "\n";
  }
  std::cout << "metric,measured,low,high,in_band\n";
  bool pass = true;
  pass &= printReferenceRow("delay_ms", 1e3 * double(delay) / fs, 0.0, 100.0);
  pass &= printReferenceRow("music_loudness_vs_source_db", measured.loudnessMusicDb,
                            -floor.loudnessMusicDb, floor.loudnessMusicDb);
  pass &= printReferenceRow("speech_loudness_vs_source_db", measured.loudnessSpeechDb,
                            -floor.loudnessSpeechDb, floor.loudnessSpeechDb);
  pass &= printReferenceRow("music_linear_100_250_db", measured.bassLinearDb,
                            floor.bassLinearDb, 100.0);
  pass &= printReferenceRow("music_linear_400_1000_db", measured.midLinearDb,
                            floor.midLinearDb, 100.0);
  pass &= printReferenceRow("music_linear_1000_3000_db", measured.presenceLinearDb,
                            floor.presenceLinearDb, 100.0);
  pass &= printReferenceRow("output_peak", peak, 0.0, 0.975);
  pass &= printReferenceRow("limiter_average_reduction_db", limiterAverageDb, 0.0, 0.5);
  pass &= printReferenceRow("power_clip", d.powerClip ? 1 : 0, 0, 0);
  pass &= printReferenceRow("speaker_clip", d.speakerClip ? 1 : 0, 0, 0);
  pass &= printReferenceRow("output_clip", d.outputClip ? 1 : 0, 0, 0);
  std::cout << "\n";
  return pass;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    HarnessConfig config = parseArgs(argc, argv);
    std::cout << std::setprecision(9);
    printConfig(config);
    if (wantsSection(config, "sweep")) runSweep(config);
    if (wantsSection(config, "envelope")) runEnvelope(config);
    if (wantsSection(config, "levels")) runLevels(config);
    if (wantsSection(config, "imd")) runImd(config);
    if (wantsSection(config, "overdrive")) runOverdrive(config);
    if (wantsSection(config, "sinad")) runSinad(config);
    if (wantsSection(config, "weak")) runWeakSignal(config);
    if (wantsSection(config, "avc")) runAvcCharacteristic(config);
    if (wantsSection(config, "sensitivity")) runSensitivity(config);
    if (config.section == "crash") runCrashRecovery(config);
    if (config.section == "preview_trace") runPreviewTrace(config);
    if (wantsSection(config, "fading")) runSelectiveFading(config);
    if (wantsSection(config, "transient")) runTransient(config);
    bool referencePassed = true;
    if (wantsSection(config, "reference")) {
      referencePassed = runReference(config);
    }
    if (wantsSection(config, "noise")) runNoise(config);
    if (wantsSection(config, "spectrum")) runSpectrum(config);
    if (wantsSection(config, "am_validate")) runAmValidate(config);
    if (wantsSection(config, "am_convergence")) runAmConvergence(config);
    if (wantsSection(config, "power_bench")) runPowerBench(config);
    bool programmePassed = true;
    if (wantsSection(config, "programme")) programmePassed = runProgramme(config);
    return referencePassed && programmePassed ? 0 : 2;
  } catch (const std::exception& ex) {
    std::cerr << "radio_measurements: " << ex.what() << "\n";
    return 1;
  }
}
