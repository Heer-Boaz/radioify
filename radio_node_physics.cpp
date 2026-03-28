#include "radio.h"

#include "audiofilter/radio1938/radio_pipeline_types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr float kDefaultSampleRate = 48000.0f;
constexpr float kDefaultBandwidthHz = 5000.0f;
constexpr float kDefaultCarrierRmsVolts = 0.12f;
constexpr uint32_t kWarmupFrames = 8192u;
constexpr uint32_t kNodeTestFrames = 4096u;
constexpr float kPhilcoIfCenterHz = 470000.0f;
constexpr float kPhilcoUndistortedOutputWatts = 15.0f;
constexpr float kPhilcoVolumeControlOhms = 2000000.0f;
constexpr float kPhilcoVolumeTapOhms = 1000000.0f;
constexpr float kPhilcoLoudnessResistanceOhms = 490000.0f;
constexpr float kPhilcoFirstAudioCouplingCapFarads = 0.01e-6f;

struct HarnessConfig {
  float sampleRate = kDefaultSampleRate;
  float bandwidthHz = kDefaultBandwidthHz;
  float carrierRmsVolts = kDefaultCarrierRmsVolts;
  float noiseWeight = 0.0f;
  std::string settingsPath;
  std::string presetName;
  std::string section = "all";
};

struct TestRow {
  std::string node;
  std::string test;
  double measured = 0.0;
  double low = 0.0;
  double high = 0.0;
  bool passed = false;
  std::string note;
};

[[noreturn]] void fail(const std::string& message) {
  throw std::runtime_error(message);
}

float parseFloatArg(char** argv, int& index, int argc, const char* name) {
  if (index + 1 >= argc) fail(std::string("missing value for ") + name);
  ++index;
  return std::strtof(argv[index], nullptr);
}

void printUsage() {
  std::cout
      << "radio_node_physics [options]\n"
      << "  --sample-rate <hz>\n"
      << "  --bw <hz>\n"
      << "  --carrier-rms <volts>\n"
      << "  --noise <value>\n"
      << "  --radio-settings <path>\n"
      << "  --preset <name>\n"
      << "  --section <all|control|rf|detector|audio|output>\n";
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
    } else if (arg == "--noise") {
      config.noiseWeight = parseFloatArg(argv, i, argc, "--noise");
    } else if (arg == "--radio-settings") {
      if (i + 1 >= argc) fail("missing value for --radio-settings");
      config.settingsPath = argv[++i];
    } else if (arg == "--preset") {
      if (i + 1 >= argc) fail("missing value for --preset");
      config.presetName = argv[++i];
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
  return config;
}

bool wantsSection(const HarnessConfig& config, std::string_view section) {
  return config.section == "all" || config.section == section;
}

double absMean(const std::vector<float>& samples, size_t startIndex) {
  if (startIndex >= samples.size()) return 0.0;
  double sum = 0.0;
  for (size_t i = startIndex; i < samples.size(); ++i) {
    sum += std::fabs(samples[i]);
  }
  return sum / static_cast<double>(samples.size() - startIndex);
}

double rmsOf(const std::vector<float>& samples, size_t startIndex = 0u) {
  if (startIndex >= samples.size()) return 0.0;
  double sumSq = 0.0;
  for (size_t i = startIndex; i < samples.size(); ++i) {
    double x = samples[i];
    sumSq += x * x;
  }
  return std::sqrt(sumSq / static_cast<double>(samples.size() - startIndex));
}

double meanOf(const std::vector<float>& samples, size_t startIndex = 0u) {
  if (startIndex >= samples.size()) return 0.0;
  double sum = 0.0;
  for (size_t i = startIndex; i < samples.size(); ++i) {
    sum += samples[i];
  }
  return sum / static_cast<double>(samples.size() - startIndex);
}

double acRmsOf(const std::vector<float>& samples, size_t startIndex = 0u) {
  if (startIndex >= samples.size()) return 0.0;
  double mean = meanOf(samples, startIndex);
  double sumSq = 0.0;
  for (size_t i = startIndex; i < samples.size(); ++i) {
    double x = samples[i] - mean;
    sumSq += x * x;
  }
  return std::sqrt(sumSq / static_cast<double>(samples.size() - startIndex));
}

double minOf(const std::vector<float>& samples) {
  if (samples.empty()) return 0.0;
  double minValue = std::numeric_limits<double>::infinity();
  for (float x : samples) minValue = std::min(minValue, static_cast<double>(x));
  return std::isfinite(minValue) ? minValue : 0.0;
}

double correlation(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size() || a.empty()) return 0.0;
  double meanA = meanOf(a);
  double meanB = meanOf(b);
  double num = 0.0;
  double denA = 0.0;
  double denB = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    double da = a[i] - meanA;
    double db = b[i] - meanB;
    num += da * db;
    denA += da * da;
    denB += db * db;
  }
  double denom = std::sqrt(std::max(denA * denB, 1e-20));
  return num / denom;
}

double effectiveDetectorAudioTauSeconds(const Radio1938& radio) {
  double storageCapFarads =
      std::max<double>(radio.demod.am.detectorStorageCapFarads, 1e-12);
  double dischargeG =
      1.0 / std::max<double>(radio.demod.am.audioDischargeResistanceOhms, 1e-6);
  double receiverLoadG =
      std::max<double>(radio.receiverCircuit.detectorLoadConductance, 0.0);
  double totalLoadG = std::max(dischargeG + receiverLoadG, 1e-12);
  return storageCapFarads / totalLoadG;
}

void addRange(std::vector<TestRow>& rows,
              const std::string& node,
              const std::string& test,
              double measured,
              double low,
              double high,
              const std::string& note = {}) {
  rows.push_back(TestRow{node, test, measured, low, high,
                         std::isfinite(measured) && measured >= low &&
                             measured <= high,
                         note});
}

void addPredicate(std::vector<TestRow>& rows,
                  const std::string& node,
                  const std::string& test,
                  bool passed,
                  double measured,
                  const std::string& note = {}) {
  rows.push_back(
      TestRow{node, test, measured, 0.0, 0.0, passed && std::isfinite(measured), note});
}

void printRows(std::string_view section, const std::vector<TestRow>& rows) {
  std::cout << "[" << section << "]\n";
  std::cout << "node,test,measured,low,high,pass,note\n";
  for (const auto& row : rows) {
    std::cout << row.node << "," << row.test << "," << row.measured << ","
              << row.low << "," << row.high << "," << (row.passed ? 1 : 0)
              << "," << row.note << "\n";
  }
  std::cout << "\n";
}

bool allPassed(const std::vector<TestRow>& rows) {
  for (const auto& row : rows) {
    if (!row.passed) return false;
  }
  return true;
}

Radio1938 makeRadio(const HarnessConfig& config,
                    float noiseWeight,
                    bool warmWithCarrier = false) {
  Radio1938 radio;
  radio.init(1, config.sampleRate, config.bandwidthHz, noiseWeight);
  if (!config.settingsPath.empty()) {
    std::string error;
    if (!applyRadioSettingsIni(radio, config.settingsPath, config.presetName,
                               &error)) {
      fail("failed to apply radio settings: " + error);
    }
  } else if (!config.presetName.empty()) {
    if (!radio.applyPreset(config.presetName)) {
      fail("unknown preset: " + config.presetName);
    }
  }
  radio.setCalibrationEnabled(true);
  if (warmWithCarrier) {
    radio.warmInputCarrier(config.carrierRmsVolts, kWarmupFrames);
  } else {
    radio.resetCalibration();
    radio.diagnostics.reset();
  }
  return radio;
}

std::vector<float> runInputNodeConstant(Radio1938& radio,
                                        float x,
                                        uint32_t frames,
                                        SourceInputMode mode) {
  std::vector<float> output(frames, 0.0f);
  RadioInputNode::reset(radio);
  for (uint32_t i = 0; i < frames; ++i) {
    RadioSampleContext ctx{};
    if (mode == SourceInputMode::RealRf) {
      ctx.signal.setRealRf(x);
    } else {
      ctx.signal.setComplexEnvelope(x, 0.0f);
    }
    output[i] = RadioInputNode::run(radio, x, ctx);
  }
  return output;
}

double runFrontEndRms(Radio1938& radio,
                      float signalHz,
                      float controlVoltage) {
  RadioInputNode::reset(radio);
  RadioFrontEndNode::reset(radio);
  radio.controlBus.controlVoltage = controlVoltage;
  std::vector<float> output(kNodeTestFrames, 0.0f);
  for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
    float t = static_cast<float>(i) / std::max(radio.sampleRate, 1.0f);
    float x = std::sin(kRadioTwoPi * signalHz * t);
    RadioSampleContext ctx{};
    ctx.signal.setRealRf(x);
    float y = RadioInputNode::run(radio, x, ctx);
    output[i] = RadioFrontEndNode::run(radio, y, ctx);
  }
  return rmsOf(output, kNodeTestFrames / 2u);
}

double runIfDetectorMagnitudeRms(Radio1938& radio, float detuneHz) {
  RadioIFStripNode::reset(radio);
  radio.frontEnd.rfGain = 1.0f;
  radio.controlBus.controlVoltage = 0.0f;
  radio.mixer.conversionGain = 1.0f;
  radio.ifStrip.appliedConfigRevision = radio.tuning.configRevision;
  radio.ifStrip.loFrequencyHz =
      radio.ifStrip.sourceCarrierHz + radio.ifStrip.ifCenterHz + detuneHz;
  std::vector<float> detectorMagnitude(kNodeTestFrames, 0.0f);
  for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
    RadioSampleContext ctx{};
    ctx.signal.setComplexEnvelope(1.0f, 0.0f);
    RadioIFStripNode::run(radio, 1.0f, ctx);
    detectorMagnitude[i] = std::sqrt(
        ctx.signal.detectorInputI * ctx.signal.detectorInputI +
        ctx.signal.detectorInputQ * ctx.signal.detectorInputQ);
  }
  return rmsOf(detectorMagnitude, kNodeTestFrames / 2u);
}

struct EnvelopeMetrics {
  double riseMs = 0.0;
  double fallMs = 0.0;
  double minValue = 0.0;
};

double findCrossingMs(const std::vector<float>& samples,
                      size_t begin,
                      size_t end,
                      double threshold,
                      bool rising,
                      float sampleRate) {
  if (begin >= end || begin >= samples.size() || sampleRate <= 0.0f) return 0.0;
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

EnvelopeMetrics measureStepEnvelope(const std::vector<float>& env,
                                    float sampleRate,
                                    size_t lowFrames,
                                    size_t highFrames) {
  EnvelopeMetrics metrics{};
  if (env.size() < lowFrames + highFrames + lowFrames) {
    metrics.minValue = minOf(env);
    return metrics;
  }

  double lowLevel = meanOf(env, lowFrames / 2u);
  double highLevel = meanOf(env, lowFrames + highFrames / 2u);
  double delta = std::max(highLevel - lowLevel, 1e-9);
  size_t riseBegin = lowFrames / 2u;
  size_t riseEnd = lowFrames + highFrames / 2u;
  size_t fallBegin = lowFrames + highFrames / 2u;
  size_t fallEnd = env.size();
  double rise10 = lowLevel + 0.10 * delta;
  double rise90 = lowLevel + 0.90 * delta;
  double t10 = findCrossingMs(env, riseBegin, riseEnd, rise10, true, sampleRate);
  double t90 = findCrossingMs(env, riseBegin, riseEnd, rise90, true, sampleRate);
  metrics.riseMs = std::max(0.0, t90 - t10);
  double fall90 = lowLevel + 0.90 * delta;
  double fall10 = lowLevel + 0.10 * delta;
  double tf90 =
      findCrossingMs(env, fallBegin, fallEnd, fall90, false, sampleRate);
  double tf10 =
      findCrossingMs(env, fallBegin, fallEnd, fall10, false, sampleRate);
  metrics.fallMs = std::max(0.0, tf10 - tf90);
  metrics.minValue = minOf(env);
  return metrics;
}

std::vector<float> runDetectorAudioStepTrace(Radio1938& radio) {
  RadioDetectorAudioNode::reset(radio);
  std::vector<float> env;
  env.reserve(3072u);
  RadioSampleContext ctx{};
  for (uint32_t i = 0; i < 512u; ++i) {
    env.push_back(RadioDetectorAudioNode::run(radio, 0.0f, ctx));
  }
  for (uint32_t i = 0; i < 512u; ++i) {
    env.push_back(RadioDetectorAudioNode::run(radio, 1.0f, ctx));
  }
  for (uint32_t i = 0; i < 2048u; ++i) {
    env.push_back(RadioDetectorAudioNode::run(radio, 0.0f, ctx));
  }
  return env;
}

struct DetectorRippleMetrics {
  double audioRippleRms = 0.0;
  double avcRippleRms = 0.0;
};

struct DemodSidebandMetrics {
  double detectorFeedRms = 0.0;
  double detectorAudioRms = 0.0;
};

DetectorRippleMetrics runDetectorRippleTrace(Radio1938& radio) {
  RadioAMDetectorNode::reset(radio);
  RadioDetectorAudioNode::reset(radio);
  std::vector<float> audioTrace;
  std::vector<float> avcTrace;
  audioTrace.reserve(kNodeTestFrames);
  avcTrace.reserve(kNodeTestFrames);
  for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
    float t = static_cast<float>(i) / std::max(radio.sampleRate, 1.0f);
    float env = 0.65f * (1.0f + 0.7f * std::sin(kRadioTwoPi * 400.0f * t));
    RadioSampleContext ctx{};
    ctx.signal.setComplexEnvelope(env, 0.0f);
    ctx.signal.detectorInputI = env;
    ctx.signal.detectorInputQ = 0.0f;
    float audioRect = RadioAMDetectorNode::run(radio, env, ctx);
    float audio = RadioDetectorAudioNode::run(radio, audioRect, ctx);
    audioTrace.push_back(audio);
    avcTrace.push_back(radio.demod.am.avcEnv);
  }
  return DetectorRippleMetrics{acRmsOf(audioTrace, kNodeTestFrames / 2u),
                               acRmsOf(avcTrace, kNodeTestFrames / 2u)};
}

DemodSidebandMetrics runIfDetectorSidebandResponse(Radio1938& radio,
                                                   float audioHz) {
  RadioMixerNode::reset(radio);
  RadioIFStripNode::reset(radio);
  RadioAMDetectorNode::reset(radio);
  RadioDetectorAudioNode::reset(radio);
  radio.controlBus.controlVoltage = 0.0f;
  radio.frontEnd.rfGain = 1.0f;
  radio.mixer.conversionGain = 1.0f;
  std::vector<float> detectorFeed;
  std::vector<float> detectorAudio;
  detectorFeed.reserve(kNodeTestFrames);
  detectorAudio.reserve(kNodeTestFrames);
  for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
    float t = static_cast<float>(i) / std::max(radio.sampleRate, 1.0f);
    float env = 1.0f + 0.5f * std::sin(kRadioTwoPi * audioHz * t);
    RadioSampleContext ctx{};
    ctx.signal.setComplexEnvelope(env, 0.0f);
    RadioIFStripNode::run(radio, env, ctx);
    float feedMag = std::sqrt(ctx.signal.detectorInputI * ctx.signal.detectorInputI +
                              ctx.signal.detectorInputQ * ctx.signal.detectorInputQ);
    float audioRect = RadioAMDetectorNode::run(radio, feedMag, ctx);
    float audio = RadioDetectorAudioNode::run(radio, audioRect, ctx);
    detectorFeed.push_back(feedMag);
    detectorAudio.push_back(audio);
  }
  return DemodSidebandMetrics{acRmsOf(detectorFeed, kNodeTestFrames / 2u),
                              acRmsOf(detectorAudio, kNodeTestFrames / 2u)};
}

DemodSidebandMetrics runRealRfDetectorSidebandResponse(Radio1938& radio,
                                                       float audioHz,
                                                       float carrierRmsVolts) {
  RadioInputNode::reset(radio);
  RadioFrontEndNode::reset(radio);
  RadioMixerNode::reset(radio);
  RadioIFStripNode::reset(radio);
  RadioAMDetectorNode::reset(radio);
  RadioDetectorAudioNode::reset(radio);
  radio.controlBus.controlVoltage = 0.0f;
  float carrierHz = radio.tuning.sourceCarrierHz;
  float carrierPeak = std::sqrt(2.0f) * std::max(carrierRmsVolts, 0.0f);
  std::vector<float> detectorFeed;
  std::vector<float> detectorAudio;
  detectorFeed.reserve(kNodeTestFrames);
  detectorAudio.reserve(kNodeTestFrames);
  for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
    float t = static_cast<float>(i) / std::max(radio.sampleRate, 1.0f);
    float env = 1.0f + 0.5f * std::sin(kRadioTwoPi * audioHz * t);
    float rf = carrierPeak * env * std::cos(kRadioTwoPi * carrierHz * t);
    RadioSampleContext ctx{};
    ctx.signal.setRealRf(rf);
    float y = RadioInputNode::run(radio, rf, ctx);
    y = RadioFrontEndNode::run(radio, y, ctx);
    y = RadioMixerNode::run(radio, y, ctx);
    y = RadioIFStripNode::run(radio, y, ctx);
    float feedMag = std::sqrt(ctx.signal.detectorInputI * ctx.signal.detectorInputI +
                              ctx.signal.detectorInputQ * ctx.signal.detectorInputQ);
    float audioRect = RadioAMDetectorNode::run(radio, y, ctx);
    float audio = RadioDetectorAudioNode::run(radio, audioRect, ctx);
    detectorFeed.push_back(feedMag);
    detectorAudio.push_back(audio);
  }
  return DemodSidebandMetrics{acRmsOf(detectorFeed, kNodeTestFrames / 2u),
                              acRmsOf(detectorAudio, kNodeTestFrames / 2u)};
}

std::vector<TestRow> runControlPhysics(const HarnessConfig& config) {
  std::vector<TestRow> rows;

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    addRange(rows, "Tuning", "source_carrier_positive", radio.tuning.sourceCarrierHz,
             1000.0, 0.49 * radio.sampleRate, "published carrier");
    addRange(rows, "Tuning", "bandwidth_clamped", radio.tuning.tunedBw,
             radio.tuning.safeBwMinHz, radio.tuning.safeBwMaxHz, "published bw");
    addRange(rows, "IFStrip", "philco_if_center_470kc", radio.ifStrip.ifCenterHz,
             469000.0, 471000.0, "Service Bulletin 258");
  }

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    RadioSampleContext dummy{};
    radio.controlSense.controlVoltageSense = -0.5f;
    RadioAVCNode::run(radio, dummy);
    addRange(rows, "AVC", "clamp_low", radio.controlBus.controlVoltage, 0.0, 0.0,
             "0..1.25 V normalized");
    radio.controlSense.controlVoltageSense = 2.0f;
    RadioAVCNode::run(radio, dummy);
    addRange(rows, "AVC", "clamp_high", radio.controlBus.controlVoltage, 1.25, 1.25,
             "0..1.25 V normalized");
  }

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    RadioSampleContext dummy{};
    radio.power.sagStart = 0.2f;
    radio.power.sagEnd = 0.8f;
    radio.controlSense.powerSagSense = 0.1f;
    RadioControlBusNode::run(radio, dummy);
    addRange(rows, "ControlBus", "sag_maps_floor", radio.controlBus.supplySag, 0.0,
             0.0, "clamped 0..1");
    radio.controlSense.powerSagSense = 1.2f;
    RadioControlBusNode::run(radio, dummy);
    addRange(rows, "ControlBus", "sag_maps_ceiling", radio.controlBus.supplySag, 1.0,
             1.0, "clamped 0..1");
  }

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    RadioSampleContext dummy{};
    radio.tuning.magneticTuningEnabled = true;
    radio.tuning.afcResponseMs = 10.0f;
    radio.tuning.afcMaxCorrectionHz = 2500.0f;
    radio.tuning.afcCaptureHz = 5000.0f;
    radio.tuning.afcDeadband = 0.0f;
    radio.tuning.tuneOffsetHz = 400.0f;
    radio.controlSense.controlVoltageSense = 1.0f;
    radio.controlSense.tuningErrorSense = 0.5f;
    for (int i = 0; i < 512; ++i) {
      RadioAFCNode::run(radio, dummy);
    }
    addPredicate(rows, "AFC", "positive_error_pulls_negative",
                 radio.tuning.afcCorrectionHz < 0.0f, radio.tuning.afcCorrectionHz,
                 "correction opposes detector error");

    Radio1938 radio2 = makeRadio(config, 0.0f, false);
    radio2.tuning.magneticTuningEnabled = true;
    radio2.tuning.afcResponseMs = 10.0f;
    radio2.tuning.afcMaxCorrectionHz = 2500.0f;
    radio2.tuning.afcCaptureHz = 5000.0f;
    radio2.tuning.afcDeadband = 0.0f;
    radio2.tuning.tuneOffsetHz = -400.0f;
    radio2.controlSense.controlVoltageSense = 1.0f;
    radio2.controlSense.tuningErrorSense = -0.5f;
    for (int i = 0; i < 512; ++i) {
      RadioAFCNode::run(radio2, dummy);
    }
    addPredicate(rows, "AFC", "negative_error_pulls_positive",
                 radio2.tuning.afcCorrectionHz > 0.0f,
                 radio2.tuning.afcCorrectionHz,
                 "correction opposes detector error");
  }

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    RadioSampleContext ctx{};
    RadioInterferenceDerivedNode::run(radio, ctx);
    addRange(rows, "InterferenceDerived", "zero_noise_zeroes_rf_noise",
             ctx.derived.demodIfNoiseAmp, 0.0, 0.0, "noiseWeight=0");
    addRange(rows, "InterferenceDerived", "zero_noise_zeroes_audio_noise",
             ctx.derived.noiseAmp, 0.0, 0.0, "noiseWeight=0");
  }

  return rows;
}

std::vector<TestRow> runRfPhysics(const HarnessConfig& config) {
  std::vector<TestRow> rows;

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    radio.globals.enableAutoLevel = false;
    double expectedDivider =
        radio.input.inputResistanceOhms /
        std::max(radio.input.sourceResistanceOhms + radio.input.inputResistanceOhms,
                 1e-9f);
    addRange(rows, "Input", "source_divider_matches_theory", radio.input.sourceDivider,
             expectedDivider - 1e-6, expectedDivider + 1e-6,
             "Rload / (Rsource + Rload)");

    auto dcResponse =
        runInputNodeConstant(radio, 1.0f, kNodeTestFrames, SourceInputMode::RealRf);
    if (radio.input.couplingCapFarads > 0.0f) {
      addRange(rows, "Input", "coupling_cap_blocks_dc", absMean(dcResponse, 3072u),
               0.0, 0.01, "tail mean abs");
    } else {
      addRange(rows, "Input", "no_coupling_cap_preserves_dc",
               meanOf(dcResponse, 3072u), 1.0 - 1e-6, 1.0 + 1e-6,
               "configured direct-coupled RF input");
    }

    auto complexBypass =
        runInputNodeConstant(radio, 0.25f, 1u, SourceInputMode::ComplexEnvelope);
    addRange(rows, "Input", "complex_envelope_bypasses_rf_coupling",
             complexBypass.front(), 0.25f - 1e-6, 0.25f + 1e-6,
             "no RF HP/divider");
  }

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    double tunedRms = runFrontEndRms(radio, radio.tuning.sourceCarrierHz, 0.0f);
    Radio1938 radioDetuned = makeRadio(config, 0.0f, false);
    float detunedSignalHz = std::min(
        radioDetuned.tuning.sourceCarrierHz + 2.0f * radioDetuned.tuning.tunedBw,
        0.45f * radioDetuned.sampleRate);
    double detunedRms =
        runFrontEndRms(radioDetuned, detunedSignalHz, 0.0f);
    addPredicate(rows, "FrontEnd", "tuned_response_exceeds_detuned",
                 tunedRms > 1.50 * std::max(detunedRms, 1e-12),
                 tunedRms / std::max(detunedRms, 1e-12),
                 "tuned/detuned RMS ratio");

    Radio1938 radioAvcLow = makeRadio(config, 0.0f, false);
    Radio1938 radioAvcHigh = makeRadio(config, 0.0f, false);
    double lowAvc = runFrontEndRms(radioAvcLow, radioAvcLow.tuning.sourceCarrierHz, 0.0f);
    double highAvc = runFrontEndRms(radioAvcHigh, radioAvcHigh.tuning.sourceCarrierHz, 1.25f);
    addPredicate(rows, "FrontEnd", "avc_reduces_rf_gain",
                 highAvc < lowAvc, highAvc / std::max(lowAvc, 1e-12),
                 "high AVC / low AVC RMS");
  }

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    RadioMixerNode::reset(radio);
    radio.controlBus.controlVoltage = 0.0f;
    RadioSampleContext ctx{};
    ctx.signal.setRealRf(0.2f);
    RadioMixerNode::run(radio, 0.2f, ctx);
    addPredicate(rows, "Mixer", "conversion_gain_finite",
                 std::isfinite(radio.mixer.conversionGain) &&
                     std::fabs(radio.mixer.conversionGain) > 1e-6f,
                 radio.mixer.conversionGain, "envelope conversion");

    Radio1938 radioLow = makeRadio(config, 0.0f, false);
    Radio1938 radioHigh = makeRadio(config, 0.0f, false);
    RadioMixerNode::reset(radioLow);
    RadioMixerNode::reset(radioHigh);
    radioLow.controlBus.controlVoltage = 0.0f;
    radioHigh.controlBus.controlVoltage = 1.25f;
    RadioSampleContext ctxLow{};
    RadioSampleContext ctxHigh{};
    ctxLow.signal.setRealRf(0.2f);
    ctxHigh.signal.setRealRf(0.2f);
    RadioMixerNode::run(radioLow, 0.2f, ctxLow);
    RadioMixerNode::run(radioHigh, 0.2f, ctxHigh);
    addPredicate(rows, "Mixer", "avc_reduces_conversion_gain",
                 std::fabs(radioHigh.mixer.conversionGain) <
                     std::fabs(radioLow.mixer.conversionGain),
                 std::fabs(radioHigh.mixer.conversionGain) /
                     std::max<double>(std::fabs(radioLow.mixer.conversionGain), 1e-12),
                 "high AVC / low AVC gain");
  }

  {
    Radio1938 radioTuned = makeRadio(config, 0.0f, false);
    Radio1938 radioDetuned = makeRadio(config, 0.0f, false);
    double tunedMag = runIfDetectorMagnitudeRms(radioTuned, 0.0f);
    double detunedMag =
        runIfDetectorMagnitudeRms(radioDetuned, 4.0f * radioDetuned.tuning.tunedBw);
    addPredicate(rows, "IFStrip", "loaded_can_prefers_tuned_envelope",
                 tunedMag > 1.25 * std::max(detunedMag, 1e-12),
                 tunedMag / std::max(detunedMag, 1e-12),
                 "tuned/detuned detector-feed RMS");

    Radio1938 radioLowSideband = makeRadio(config, 0.0f, false);
    Radio1938 radioHighSideband = makeRadio(config, 0.0f, false);
    auto low = runIfDetectorSidebandResponse(radioLowSideband, 100.0f);
    auto high = runIfDetectorSidebandResponse(radioHighSideband, 5000.0f);
    addPredicate(rows, "IFStrip", "detector_feed_supports_5khz_sideband",
                 high.detectorFeedRms > 0.25 * std::max(low.detectorFeedRms, 1e-12),
                 high.detectorFeedRms / std::max(low.detectorFeedRms, 1e-12),
                 "5 kHz / 100 Hz detector-feed ripple");
    addPredicate(rows, "IFStrip", "detector_audio_supports_5khz_sideband",
                 high.detectorAudioRms > 0.20 * std::max(low.detectorAudioRms, 1e-12),
                 high.detectorAudioRms / std::max(low.detectorAudioRms, 1e-12),
                 "5 kHz / 100 Hz detector-audio ripple");

    Radio1938 radioRfLow = makeRadio(config, 0.0f, false);
    Radio1938 radioRfHigh = makeRadio(config, 0.0f, false);
    auto rfLow =
        runRealRfDetectorSidebandResponse(radioRfLow, 100.0f, config.carrierRmsVolts);
    auto rfHigh =
        runRealRfDetectorSidebandResponse(radioRfHigh, 5000.0f, config.carrierRmsVolts);
    addPredicate(rows, "RFChain", "real_rf_detector_feed_supports_5khz_sideband",
                 rfHigh.detectorFeedRms > 0.50 * std::max(rfLow.detectorFeedRms, 1e-12),
                 rfHigh.detectorFeedRms / std::max(rfLow.detectorFeedRms, 1e-12),
                 "5 kHz / 100 Hz detector-feed ripple");
    addPredicate(rows, "RFChain", "real_rf_detector_audio_supports_5khz_sideband",
                 rfHigh.detectorAudioRms > 0.50 * std::max(rfLow.detectorAudioRms, 1e-12),
                 rfHigh.detectorAudioRms / std::max(rfLow.detectorAudioRms, 1e-12),
                 "5 kHz / 100 Hz detector-audio ripple");
  }

  return rows;
}

std::vector<TestRow> runDetectorPhysics(const HarnessConfig& config) {
  std::vector<TestRow> rows;

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    RadioAMDetectorNode::reset(radio);
    std::vector<float> detectorNodes;
    detectorNodes.reserve(kNodeTestFrames);
    for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
      float t = static_cast<float>(i) / std::max(radio.sampleRate, 1.0f);
      float env = 0.18f * (1.0f + 0.7f * std::sin(kRadioTwoPi * 400.0f * t));
      RadioSampleContext ctx{};
      ctx.signal.setComplexEnvelope(env, 0.0f);
      ctx.signal.detectorInputI = env;
      ctx.signal.detectorInputQ = 0.0f;
      RadioAMDetectorNode::run(radio, env, ctx);
      detectorNodes.push_back(radio.demod.am.detectorNode);
    }
    addPredicate(rows, "AMDetector", "detector_node_nonnegative",
                 minOf(detectorNodes) >= -1e-7, minOf(detectorNodes),
                 "diode+RC node");

    Radio1938 weak = makeRadio(config, 0.0f, false);
    Radio1938 strong = makeRadio(config, 0.0f, false);
    RadioAMDetectorNode::reset(weak);
    RadioAMDetectorNode::reset(strong);
    for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
      RadioSampleContext weakCtx{};
      RadioSampleContext strongCtx{};
      weakCtx.signal.setComplexEnvelope(0.04f, 0.0f);
      strongCtx.signal.setComplexEnvelope(0.90f, 0.0f);
      weakCtx.signal.detectorInputI = 0.04f;
      strongCtx.signal.detectorInputI = 0.90f;
      RadioAMDetectorNode::run(weak, 0.04f, weakCtx);
      RadioAMDetectorNode::run(strong, 0.90f, strongCtx);
    }
    addPredicate(rows, "AMDetector", "stronger_input_charges_more_avc",
                 strong.demod.am.avcEnv > weak.demod.am.avcEnv,
                 strong.demod.am.avcEnv - weak.demod.am.avcEnv,
                 "strong minus weak AVC node");
  }

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    auto env = runDetectorAudioStepTrace(radio);
    EnvelopeMetrics metrics =
        measureStepEnvelope(env, radio.sampleRate, 512u, 512u);
    double detectorTauUs = 1e6 * effectiveDetectorAudioTauSeconds(radio);
    double carrierPeriodUs =
        1e6 / std::max<double>(radio.ifStrip.ifCenterHz, 1.0);
    double sidebandPeriodUs =
        1e6 / std::max<double>(radio.tuning.tunedBw, 1.0);
    double avcTauMs =
        1e3 * std::max<double>(radio.demod.am.avcDischargeResistanceOhms, 1e-6) *
        std::max<double>(radio.demod.am.avcFilterCapFarads, 1e-12);
    addPredicate(rows, "DetectorAudio", "audio_env_nonnegative",
                 metrics.minValue >= -1e-7, metrics.minValue,
                 "rectified audio branch");
    addRange(rows, "DetectorAudio", "attack_ms", metrics.riseMs, 0.0, 2.0,
             "10-90 step attack");
    addRange(rows, "DetectorAudio", "release_ms", metrics.fallMs, 0.0, 20.0,
             "90-10 step release");
    addPredicate(rows, "DetectorAudio", "release_slower_than_attack",
                 metrics.fallMs > metrics.riseMs, metrics.fallMs - metrics.riseMs,
                 "fall_ms - rise_ms");
    addPredicate(rows, "DetectorAudio", "tau_exceeds_if_cycles",
                 detectorTauUs > 4.0 * carrierPeriodUs, detectorTauUs,
                 "effective detector tau (us)");
    addPredicate(rows, "DetectorAudio", "tau_stays_inside_service_sideband",
                 detectorTauUs < 0.75 * sidebandPeriodUs, detectorTauUs,
                 "effective detector tau (us)");
    addRange(rows, "AMDetector", "avc_tau_ms", avcTauMs, 10.0, 500.0,
             "AVC release RC");
    addPredicate(rows, "AMDetector", "avc_tau_decades_slower_than_audio_tau",
                 avcTauMs > 50.0 * (detectorTauUs * 1e-3), avcTauMs,
                 "AVC tau / detector tau");
  }

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    auto ripple = runDetectorRippleTrace(radio);
    addPredicate(rows, "AMDetector", "avc_rejects_audio_ripple",
                 ripple.avcRippleRms < 0.10 * std::max(ripple.audioRippleRms, 1e-12),
                 ripple.avcRippleRms / std::max(ripple.audioRippleRms, 1e-12),
                 "AVC ripple / detector-audio ripple");
  }

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    RadioReceiverInputNetworkNode::reset(radio);
    addRange(rows, "ReceiverInputNetwork", "philco_volume_total_2meg",
             radio.receiverCircuit.volumeControlResistanceOhms, 1.9e6, 2.1e6,
             "Service Bulletin 258");
    addRange(rows, "ReceiverInputNetwork", "philco_volume_tap_1meg",
             radio.receiverCircuit.volumeControlTapResistanceOhms, 0.95e6, 1.05e6,
             "Service Bulletin 258");
    addRange(rows, "ReceiverInputNetwork", "philco_loudness_resistor_490k",
             radio.receiverCircuit.volumeControlLoudnessResistanceOhms, 0.46e6,
             0.52e6, "Service Bulletin 258");
    addRange(rows, "ReceiverInputNetwork", "philco_first_audio_coupling_0p01uf",
             radio.receiverCircuit.couplingCapFarads, 9e-9, 11e-9,
             "Service Bulletin 258");
    addPredicate(rows, "ReceiverInputNetwork", "detector_load_positive_finite",
                 std::isfinite(radio.receiverCircuit.detectorLoadConductance) &&
                     radio.receiverCircuit.detectorLoadConductance > 0.0f,
                 radio.receiverCircuit.detectorLoadConductance,
                 "equivalent detector load conductance");

    for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
      radio.detectorAudio.audioEnv = 1.0f;
      RadioSampleContext ctx{};
      RadioReceiverInputNetworkNode::run(radio, 1.0f, ctx);
    }
    addRange(rows, "ReceiverInputNetwork", "grid_blocks_detector_dc",
             std::fabs(radio.receiverCircuit.gridVoltage), 0.0, 0.05,
             "steady-state grid AC volts");
    addRange(rows, "ReceiverInputNetwork", "tap_stays_between_ground_and_detector",
             radio.receiverCircuit.volumeControlTapVoltage, 0.0, 1.0,
             "steady-state tap voltage");
  }

  return rows;
}

std::vector<TestRow> runAudioPhysics(const HarnessConfig& config) {
  std::vector<TestRow> rows;

  {
    Radio1938 radioNeg = makeRadio(config, 0.0f, false);
    Radio1938 radioPos = makeRadio(config, 0.0f, false);
    radioNeg.receiverCircuit.warmStartPending = false;
    radioPos.receiverCircuit.warmStartPending = false;
    radioNeg.receiverCircuit.gridVoltage = -0.2f;
    radioPos.receiverCircuit.gridVoltage = 0.2f;
    radioNeg.receiverCircuit.tubePlateVoltage =
        radioNeg.receiverCircuit.tubeQuiescentPlateVolts;
    radioPos.receiverCircuit.tubePlateVoltage =
        radioPos.receiverCircuit.tubeQuiescentPlateVolts;
    RadioSampleContext ctx{};
    RadioReceiverCircuitNode::run(radioNeg, 0.0f, ctx);
    RadioReceiverCircuitNode::run(radioPos, 0.0f, ctx);
    addPredicate(rows, "ReceiverCircuit", "positive_grid_pulls_plate_down",
                 radioPos.receiverCircuit.tubePlateVoltage <
                     radioNeg.receiverCircuit.tubePlateVoltage,
                 radioNeg.receiverCircuit.tubePlateVoltage -
                     radioPos.receiverCircuit.tubePlateVoltage,
                 "plate V at -0.2 grid minus +0.2 grid");
  }

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    addPredicate(rows, "Power", "nominal_output_reference_positive",
                 std::isfinite(radio.power.nominalOutputPowerWatts) &&
                     radio.power.nominalOutputPowerWatts > 0.0f &&
                     std::isfinite(radio.output.digitalReferenceSpeakerVoltsPeak) &&
                     radio.output.digitalReferenceSpeakerVoltsPeak > 0.0f,
                 radio.output.digitalReferenceSpeakerVoltsPeak,
                 "derived from output stage");
    addRange(rows, "Power", "philco_undistorted_output_near_15w",
             radio.power.nominalOutputPowerWatts, 12.0, 18.0,
             "Service Bulletin 258");

    RadioPowerNode::reset(radio);
    std::vector<float> secondaryZero(kNodeTestFrames, 0.0f);
    RadioSampleContext ctx{};
    for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
      secondaryZero[i] = RadioPowerNode::run(radio, 0.0f, ctx);
    }
    addRange(rows, "Power", "zero_drive_has_near_zero_secondary",
             rmsOf(secondaryZero, kNodeTestFrames / 2u), 0.0, 1e-3,
             "secondary RMS at zero drive");

    Radio1938 radioDc = makeRadio(config, 0.0f, false);
    RadioPowerNode::reset(radioDc);
    for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
      RadioPowerNode::run(radioDc, 10.0f, ctx);
    }
    addRange(rows, "Power", "driver_coupling_cap_blocks_dc",
             std::fabs(radioDc.power.gridVoltage), 0.0, 0.05,
             "steady-state driver grid AC volts");

    Radio1938 radioDrive = makeRadio(config, 0.0f, false);
    RadioPowerNode::reset(radioDrive);
    std::vector<float> gridA;
    std::vector<float> gridB;
    gridA.reserve(kNodeTestFrames);
    gridB.reserve(kNodeTestFrames);
    for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
      float t = static_cast<float>(i) / std::max(radioDrive.sampleRate, 1.0f);
      float driver = 20.0f * std::sin(kRadioTwoPi * 400.0f * t);
      RadioPowerNode::run(radioDrive, driver, ctx);
      gridA.push_back(radioDrive.power.outputGridAVolts);
      gridB.push_back(radioDrive.power.outputGridBVolts);
    }
    addRange(rows, "Power", "output_grids_are_antiphase", correlation(gridA, gridB),
             -1.0, -0.30, "A/B grid correlation");
  }

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    RadioToneNode::reset(radio);
    std::vector<float> toneOut(kNodeTestFrames, 0.0f);
    RadioSampleContext ctx{};
    for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
      toneOut[i] = RadioToneNode::run(radio, 0.0f, ctx);
    }
    addRange(rows, "Tone", "zero_input_stays_zero", rmsOf(toneOut), 0.0, 1e-7,
             "passive tone shaping");
  }

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    RadioNoiseNode::reset(radio);
    RadioSampleContext ctx{};
    ctx.derived = {};
    float y = RadioNoiseNode::run(radio, 0.125f, ctx);
    addRange(rows, "Noise", "zero_derived_noise_is_passthrough", y,
             0.125f - 1e-7, 0.125f + 1e-7, "no derived hiss/crackle/hum");
  }

  return rows;
}

std::vector<TestRow> runOutputPhysics(const HarnessConfig& config) {
  std::vector<TestRow> rows;

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    RadioSpeakerNode::reset(radio);
    std::vector<float> out(kNodeTestFrames, 0.0f);
    RadioSampleContext ctx{};
    for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
      out[i] = RadioSpeakerNode::run(radio, 0.0f, ctx);
    }
    addRange(rows, "Speaker", "zero_input_stays_zero", rmsOf(out), 0.0, 1e-7,
             "no spontaneous motion");
  }

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    RadioCabinetNode::reset(radio);
    std::vector<float> out(kNodeTestFrames, 0.0f);
    RadioSampleContext ctx{};
    for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
      out[i] = RadioCabinetNode::run(radio, 0.0f, ctx);
    }
    addRange(rows, "Cabinet", "zero_input_stays_zero", rmsOf(out), 0.0, 1e-7,
             "passive cabinet coloration");
  }

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    RadioSampleContext ctx{};
    float scaled = RadioOutputScaleNode::run(
        radio, radio.output.digitalReferenceSpeakerVoltsPeak, ctx);
    addRange(rows, "OutputScale", "reference_maps_to_headroom_peak", scaled,
             (1.0 / 1.12) - 1e-6, (1.0 / 1.12) + 1e-6,
             "physical speaker volts to digital");
  }

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    RadioInitContext initCtx{};
    radio.finalLimiter.enabled = true;
    radio.finalLimiter.threshold = 0.2f;
    radio.finalLimiter.lookaheadMs = 0.0f;
    radio.finalLimiter.attackMs = 0.1f;
    radio.finalLimiter.releaseMs = 5.0f;
    RadioFinalLimiterNode::init(radio, initCtx);
    RadioFinalLimiterNode::reset(radio);
    RadioSampleContext ctx{};
    float out = 0.0f;
    for (int i = 0; i < 64; ++i) {
      out = RadioFinalLimiterNode::run(radio, 1.0f, ctx);
    }
    addPredicate(rows, "FinalLimiter", "large_input_reduces_gain",
                 std::fabs(out) < 1.0f &&
                     radio.diagnostics.finalLimiterActiveSamples > 0,
                 out, "safety stage only");
  }

  {
    Radio1938 radio = makeRadio(config, 0.0f, false);
    RadioInitContext initCtx{};
    radio.globals.outputClipThreshold = 0.5f;
    RadioOutputClipNode::init(radio, initCtx);
    RadioOutputClipNode::reset(radio);
    RadioSampleContext ctx{};
    float out = RadioOutputClipNode::run(radio, 1.0f, ctx);
    addPredicate(rows, "OutputClip", "clip_threshold_marks_and_clamps",
                 radio.diagnostics.outputClipSamples > 0 && std::fabs(out) <= 0.5f,
                 out, "hard safety clamp");
  }

  return rows;
}

void printConfig(const HarnessConfig& config) {
  std::cout << "[config]\n";
  std::cout << "key,value\n";
  std::cout << "sample_rate_hz," << config.sampleRate << "\n";
  std::cout << "bandwidth_hz," << config.bandwidthHz << "\n";
  std::cout << "carrier_rms_volts," << config.carrierRmsVolts << "\n";
  std::cout << "noise_weight," << config.noiseWeight << "\n\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    HarnessConfig config = parseArgs(argc, argv);
    std::cout << std::setprecision(9);
    printConfig(config);

    bool ok = true;
    if (wantsSection(config, "control")) {
      auto rows = runControlPhysics(config);
      printRows("control", rows);
      ok = ok && allPassed(rows);
    }
    if (wantsSection(config, "rf")) {
      auto rows = runRfPhysics(config);
      printRows("rf", rows);
      ok = ok && allPassed(rows);
    }
    if (wantsSection(config, "detector")) {
      auto rows = runDetectorPhysics(config);
      printRows("detector", rows);
      ok = ok && allPassed(rows);
    }
    if (wantsSection(config, "audio")) {
      auto rows = runAudioPhysics(config);
      printRows("audio", rows);
      ok = ok && allPassed(rows);
    }
    if (wantsSection(config, "output")) {
      auto rows = runOutputPhysics(config);
      printRows("output", rows);
      ok = ok && allPassed(rows);
    }
    return ok ? 0 : 2;
  } catch (const std::exception& ex) {
    std::cerr << "radio_node_physics: " << ex.what() << "\n";
    return 1;
  }
}
