#include "radio.h"

#include "audiofilter/radio1938/audio_pipeline.h"
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
  std::string node = "all";
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
      << "  --section <all|control|rf|detector|audio|output>\n"
      << "  --node <all|Tuning|Input|AVC|AFC|ControlBus|InterferenceDerived|"
         "FrontEnd|Mixer|IFStrip|AMDetector|DetectorAudio|"
         "ReceiverInputNetwork|ReceiverCircuit|Tone|Power|Noise|"
         "Speaker|Cabinet|OutputScale|FinalLimiter|OutputClip>\n";
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
    } else if (arg == "--node") {
      if (i + 1 >= argc) fail("missing value for --node");
      config.node = argv[++i];
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

constexpr std::array<std::pair<PassId, const char*>, 21> kNodeLabels{{
    {PassId::Tuning, "Tuning"},
    {PassId::Input, "Input"},
    {PassId::AVC, "AVC"},
    {PassId::AFC, "AFC"},
    {PassId::ControlBus, "ControlBus"},
    {PassId::InterferenceDerived, "InterferenceDerived"},
    {PassId::FrontEnd, "FrontEnd"},
    {PassId::Mixer, "Mixer"},
    {PassId::IFStrip, "IFStrip"},
    {PassId::Demod, "AMDetector"},
    {PassId::DetectorAudio, "DetectorAudio"},
    {PassId::ReceiverInputNetwork, "ReceiverInputNetwork"},
    {PassId::ReceiverCircuit, "ReceiverCircuit"},
    {PassId::Tone, "Tone"},
    {PassId::Power, "Power"},
    {PassId::Noise, "Noise"},
    {PassId::Speaker, "Speaker"},
    {PassId::Cabinet, "Cabinet"},
    {PassId::OutputScale, "OutputScale"},
    {PassId::FinalLimiter, "FinalLimiter"},
    {PassId::OutputClip, "OutputClip"},
}};

bool isKnownNode(std::string_view node) {
  if (node == "all") return true;
  for (const auto& [_, label] : kNodeLabels) {
    if (node == label) return true;
  }
  return false;
}

bool wantsNode(const HarnessConfig& config, std::string_view node) {
  return config.node == "all" || config.node == node;
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

double positivePeakOf(const std::vector<float>& samples, size_t startIndex = 0u) {
  if (startIndex >= samples.size()) return 0.0;
  double peak = 0.0;
  for (size_t i = startIndex; i < samples.size(); ++i) {
    peak = std::max(peak, static_cast<double>(samples[i]));
  }
  return peak;
}

double negativePeakAbsOf(const std::vector<float>& samples,
                         size_t startIndex = 0u) {
  if (startIndex >= samples.size()) return 0.0;
  double peak = 0.0;
  for (size_t i = startIndex; i < samples.size(); ++i) {
    peak = std::max(peak, static_cast<double>(-samples[i]));
  }
  return peak;
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

std::vector<TestRow> filterRowsForNode(const HarnessConfig& config,
                                       std::vector<TestRow> rows) {
  if (config.node == "all") return rows;
  rows.erase(std::remove_if(rows.begin(), rows.end(),
                            [&](const TestRow& row) {
                              return row.node != config.node;
                            }),
             rows.end());
  return rows;
}

bool allPassed(const std::vector<TestRow>& rows) {
  for (const auto& row : rows) {
    if (!row.passed) return false;
  }
  return true;
}

Radio1938 makeUninitializedFixture(const HarnessConfig& config, float noiseWeight) {
  Radio1938 radio;
  radio.channels = 1;
  radio.sampleRate = config.sampleRate;
  radio.bwHz = config.bandwidthHz;
  radio.noiseWeight = noiseWeight;
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
  radio.initialized = true;
  radio.setCalibrationEnabled(true);
  radio.resetCalibration();
  radio.diagnostics.reset();
  radio.controlSense.reset();
  radio.controlBus.reset();
  radio.iqInput.resetRuntime();
  return radio;
}

void initPassRecursive(const Radio1938AudioPipeline& pipeline,
                       Radio1938& radio,
                       PassId id,
                       std::vector<bool>& initialized,
                       RadioInitContext& initCtx) {
  const size_t index = static_cast<size_t>(id);
  if (index >= initialized.size() || initialized[index]) return;
  const auto* pass = pipeline.findPass(id);
  if (!pass) fail("missing pipeline pass fixture");
  for (size_t i = 0; i < pass->dependencies.count; ++i) {
    initPassRecursive(pipeline, radio, pass->dependencies.ids[i], initialized, initCtx);
  }
  if (pass->init) pass->init(radio, initCtx);
  initialized[index] = true;
}

void resetPassRecursive(const Radio1938AudioPipeline& pipeline,
                        Radio1938& radio,
                        PassId id,
                        std::vector<bool>& reset,
                        std::vector<bool>& visiting) {
  const size_t index = static_cast<size_t>(id);
  if (index >= reset.size() || reset[index] || visiting[index]) return;
  const auto* pass = pipeline.findPass(id);
  if (!pass) fail("missing pipeline pass fixture");
  visiting[index] = true;
  for (size_t other = 0; other < static_cast<size_t>(PassId::OutputClip) + 1u; ++other) {
    const auto* candidate = pipeline.findPass(static_cast<PassId>(other));
    if (!candidate) continue;
    for (size_t dep = 0; dep < candidate->dependencies.count; ++dep) {
      if (candidate->dependencies.ids[dep] == id) {
        resetPassRecursive(pipeline, radio, candidate->id, reset, visiting);
        break;
      }
    }
  }
  if (pass->reset) pass->reset(radio);
  reset[index] = true;
  visiting[index] = false;
}

Radio1938 makeNodeFixture(const HarnessConfig& config,
                          float noiseWeight,
                          std::initializer_list<PassId> passes) {
  Radio1938 radio = makeUninitializedFixture(config, noiseWeight);
  const auto& pipeline = radio1938AudioPipeline();
  RadioInitContext initCtx{};
  std::vector<bool> initialized(static_cast<size_t>(PassId::OutputClip) + 1u, false);
  initPassRecursive(pipeline, radio, PassId::Tuning, initialized, initCtx);
  for (PassId id : passes) {
    initPassRecursive(pipeline, radio, id, initialized, initCtx);
  }
  std::vector<bool> reset(static_cast<size_t>(PassId::OutputClip) + 1u, false);
  std::vector<bool> visiting(static_cast<size_t>(PassId::OutputClip) + 1u, false);
  for (PassId id : passes) {
    resetPassRecursive(pipeline, radio, id, reset, visiting);
  }
  radio.graph.compile();
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

std::vector<float> runSpeakerTone(Radio1938& radio,
                                  float frequencyHz,
                                  float peakAmplitude) {
  RadioInitContext initCtx{};
  RadioSpeakerNode::init(radio, initCtx);
  RadioSpeakerNode::reset(radio);
  std::vector<float> output(kNodeTestFrames, 0.0f);
  for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
    float t = static_cast<float>(i) / std::max(radio.sampleRate, 1.0f);
    float x = peakAmplitude * std::sin(kRadioTwoPi * frequencyHz * t);
    RadioSampleContext ctx{};
    output[i] = RadioSpeakerNode::run(radio, x, ctx);
  }
  return output;
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
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::Tuning, PassId::IFStrip});
    addRange(rows, "Tuning", "source_carrier_positive", radio.tuning.sourceCarrierHz,
             1000.0, 0.49 * radio.sampleRate, "published carrier");
    addRange(rows, "Tuning", "bandwidth_clamped", radio.tuning.tunedBw,
             radio.tuning.safeBwMinHz, radio.tuning.safeBwMaxHz, "published bw");
    addRange(rows, "IFStrip", "philco_if_center_470kc", radio.ifStrip.ifCenterHz,
             469000.0, 471000.0, "Service Bulletin 258");
  }

  {
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::Tuning});
    radio.tuning.tuneOffsetHz = 0.75f * radio.tuning.tunedBw;
    RadioBlockControl block{};
    RadioTuningNode::prepare(radio, block, 2048u);
    addPredicate(rows, "Tuning", "prepare_publishes_positive_tune_norm",
                 block.tuneNorm > 0.0f && block.tuneNorm <= 1.0f, block.tuneNorm,
                 "normalized mistune follows tuning sign");
  }

  {
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::AVC});
    RadioSampleContext dummy{};
    radio.controlSense.controlVoltageSense = -0.5f;
    RadioAVCNode::run(radio, dummy);
    addRange(rows, "AVC", "clamp_low", radio.controlBus.controlVoltage, 0.0, 0.0,
             "0..1.25 V normalized");
    radio.controlSense.controlVoltageSense = 2.0f;
    RadioAVCNode::run(radio, dummy);
    addRange(rows, "AVC", "clamp_high", radio.controlBus.controlVoltage, 1.25, 1.25,
             "0..1.25 V normalized");
    radio.controlSense.controlVoltageSense = 0.6f;
    RadioAVCNode::run(radio, dummy);
    addRange(rows, "AVC", "midrange_is_identity", radio.controlBus.controlVoltage,
             0.6f - 1e-6, 0.6f + 1e-6, "unsaturated AVC must pass through");
  }

  {
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::ControlBus});
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
    radio.controlSense.powerSagSense = 0.5f * (radio.power.sagStart + radio.power.sagEnd);
    RadioControlBusNode::run(radio, dummy);
    addRange(rows, "ControlBus", "sag_midpoint_maps_half_scale",
             radio.controlBus.supplySag, 0.49, 0.51,
             "linear map between sagStart and sagEnd");
  }

  {
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::AFC});
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

    Radio1938 radio2 = makeNodeFixture(config, 0.0f, {PassId::AFC});
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

    Radio1938 radio3 = makeNodeFixture(config, 0.0f, {PassId::AFC});
    radio3.tuning.magneticTuningEnabled = false;
    radio3.tuning.afcMaxCorrectionHz = 2500.0f;
    radio3.tuning.afcResponseMs = 10.0f;
    radio3.controlSense.tuningErrorSense = 0.8f;
    RadioAFCNode::run(radio3, dummy);
    addRange(rows, "AFC", "disabled_afc_forces_zero_correction",
             radio3.tuning.afcCorrectionHz, 0.0, 0.0,
             "no magnetic tuning means no AFC correction");
  }

  {
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::InterferenceDerived});
    RadioSampleContext ctx{};
    RadioInterferenceDerivedNode::run(radio, ctx);
    addRange(rows, "InterferenceDerived", "zero_noise_zeroes_rf_noise",
             ctx.derived.demodIfNoiseAmp, 0.0, 0.0, "noiseWeight=0");
    addRange(rows, "InterferenceDerived", "zero_noise_zeroes_audio_noise",
             ctx.derived.noiseAmp, 0.0, 0.0, "noiseWeight=0");
  }

  {
    Radio1938 tuned = makeNodeFixture(config, 0.4f, {PassId::InterferenceDerived});
    Radio1938 mistuned = makeNodeFixture(config, 0.4f, {PassId::InterferenceDerived});
    tuned.controlBus.controlVoltage = 1.0f;
    tuned.controlSense.tuningErrorSense = 0.0f;
    mistuned.controlBus.controlVoltage = 1.0f;
    mistuned.controlSense.tuningErrorSense = 1.0f;
    tuned.tuning.tuneAppliedHz = 0.0f;
    mistuned.tuning.tuneAppliedHz = mistuned.tuning.tunedBw;
    RadioSampleContext tunedCtx{};
    RadioSampleContext mistunedCtx{};
    RadioInterferenceDerivedNode::run(tuned, tunedCtx);
    RadioInterferenceDerivedNode::run(mistuned, mistunedCtx);
    addPredicate(rows, "InterferenceDerived", "mistune_increases_detector_crackle",
                 mistunedCtx.derived.demodIfCrackleAmp >
                     tunedCtx.derived.demodIfCrackleAmp,
                 mistunedCtx.derived.demodIfCrackleAmp -
                     tunedCtx.derived.demodIfCrackleAmp,
                 "detector crackle exposure rises when capture is lost");
  }

  return rows;
}

std::vector<TestRow> runRfPhysics(const HarnessConfig& config) {
  std::vector<TestRow> rows;

  {
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::Input});
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
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::FrontEnd});
    double tunedRms = runFrontEndRms(radio, radio.tuning.sourceCarrierHz, 0.0f);
    Radio1938 radioDetuned = makeNodeFixture(config, 0.0f, {PassId::FrontEnd});
    float detunedSignalHz = std::min(
        radioDetuned.tuning.sourceCarrierHz + 2.0f * radioDetuned.tuning.tunedBw,
        0.45f * radioDetuned.sampleRate);
    double detunedRms =
        runFrontEndRms(radioDetuned, detunedSignalHz, 0.0f);
    addPredicate(rows, "FrontEnd", "tuned_response_exceeds_detuned",
                 tunedRms > 1.50 * std::max(detunedRms, 1e-12),
                 tunedRms / std::max(detunedRms, 1e-12),
                 "tuned/detuned RMS ratio");

    Radio1938 radioAvcLow = makeNodeFixture(config, 0.0f, {PassId::FrontEnd});
    Radio1938 radioAvcHigh = makeNodeFixture(config, 0.0f, {PassId::FrontEnd});
    double lowAvc = runFrontEndRms(radioAvcLow, radioAvcLow.tuning.sourceCarrierHz, 0.0f);
    double highAvc = runFrontEndRms(radioAvcHigh, radioAvcHigh.tuning.sourceCarrierHz, 1.25f);
    addPredicate(rows, "FrontEnd", "avc_reduces_rf_gain",
                 highAvc < lowAvc, highAvc / std::max(lowAvc, 1e-12),
                 "high AVC / low AVC RMS");
  }

  {
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::Mixer});
    RadioMixerNode::reset(radio);
    radio.controlBus.controlVoltage = 0.0f;
    RadioSampleContext ctx{};
    ctx.signal.setRealRf(0.2f);
    RadioMixerNode::run(radio, 0.2f, ctx);
    addPredicate(rows, "Mixer", "conversion_gain_finite",
                 std::isfinite(radio.mixer.conversionGain) &&
                     std::fabs(radio.mixer.conversionGain) > 1e-6f,
                 radio.mixer.conversionGain, "envelope conversion");

    Radio1938 radioLow = makeNodeFixture(config, 0.0f, {PassId::Mixer});
    Radio1938 radioHigh = makeNodeFixture(config, 0.0f, {PassId::Mixer});
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

    Radio1938 iq = makeNodeFixture(config, 0.0f, {PassId::Mixer});
    RadioMixerNode::reset(iq);
    RadioSampleContext iqCtx{};
    iqCtx.signal.setComplexEnvelope(0.31f, -0.27f);
    float iqOut = RadioMixerNode::run(iq, 0.31f, iqCtx);
    addRange(rows, "Mixer", "complex_envelope_path_is_i_passthrough", iqOut,
             0.31f - 1e-6, 0.31f + 1e-6, "IQ mode bypasses real-RF remodulation");
  }

  {
    Radio1938 radioTuned = makeNodeFixture(config, 0.0f, {PassId::IFStrip});
    Radio1938 radioDetuned = makeNodeFixture(config, 0.0f, {PassId::IFStrip});
    double tunedMag = runIfDetectorMagnitudeRms(radioTuned, 0.0f);
    double detunedMag =
        runIfDetectorMagnitudeRms(radioDetuned, 4.0f * radioDetuned.tuning.tunedBw);
    addPredicate(rows, "IFStrip", "loaded_can_prefers_tuned_envelope",
                 tunedMag > 1.25 * std::max(detunedMag, 1e-12),
                 tunedMag / std::max(detunedMag, 1e-12),
                 "tuned/detuned detector-feed RMS");

    Radio1938 radioLowSideband =
        makeNodeFixture(config, 0.0f, {PassId::IFStrip, PassId::Demod, PassId::DetectorAudio});
    Radio1938 radioHighSideband =
        makeNodeFixture(config, 0.0f, {PassId::IFStrip, PassId::Demod, PassId::DetectorAudio});
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

    Radio1938 radioRfLow =
        makeNodeFixture(config, 0.0f, {PassId::FrontEnd, PassId::Mixer, PassId::IFStrip,
                                       PassId::Demod, PassId::DetectorAudio});
    Radio1938 radioRfHigh =
        makeNodeFixture(config, 0.0f, {PassId::FrontEnd, PassId::Mixer, PassId::IFStrip,
                                       PassId::Demod, PassId::DetectorAudio});
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
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::Demod});
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

    Radio1938 weak = makeNodeFixture(config, 0.0f, {PassId::Demod});
    Radio1938 strong = makeNodeFixture(config, 0.0f, {PassId::Demod});
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
    Radio1938 slowerAfc = makeNodeFixture(config, 0.0f, {PassId::Demod});
    Radio1938 fasterAfc = makeNodeFixture(config, 0.0f, {PassId::Demod});
    slowerAfc.demod.am.afcSenseLpHz = 34.0f;
    fasterAfc.demod.am.afcSenseLpHz = 340.0f;
    slowerAfc.demod.am.setBandwidth(slowerAfc.demod.am.bwHz,
                                    slowerAfc.demod.am.tuneOffsetHz);
    fasterAfc.demod.am.setBandwidth(fasterAfc.demod.am.bwHz,
                                    fasterAfc.demod.am.tuneOffsetHz);
    addPredicate(rows, "AMDetector", "afc_sense_lp_hz_controls_probe_bandwidth",
                 slowerAfc.demod.am.afcLowProbe.i.b0 <
                     fasterAfc.demod.am.afcLowProbe.i.b0,
                 fasterAfc.demod.am.afcLowProbe.i.b0 /
                     std::max<double>(slowerAfc.demod.am.afcLowProbe.i.b0, 1e-12),
                 "fast/slow AFC probe b0 ratio");
  }

  {
    Radio1938 lowThreshold = makeNodeFixture(config, 0.0f, {PassId::Demod});
    Radio1938 highThreshold = makeNodeFixture(config, 0.0f, {PassId::Demod});
    lowThreshold.demod.am.avcDiodeDrop = 0.0f;
    lowThreshold.demod.am.avcJunctionSlopeVolts = 0.002f;
    highThreshold.demod.am.avcDiodeDrop = 0.18f;
    highThreshold.demod.am.avcJunctionSlopeVolts = 0.020f;
    RadioAMDetectorNode::reset(lowThreshold);
    RadioAMDetectorNode::reset(highThreshold);
    for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
      RadioSampleContext lowCtx{};
      RadioSampleContext highCtx{};
      lowCtx.signal.setComplexEnvelope(0.90f, 0.0f);
      highCtx.signal.setComplexEnvelope(0.90f, 0.0f);
      lowCtx.signal.detectorInputI = 0.90f;
      highCtx.signal.detectorInputI = 0.90f;
      RadioAMDetectorNode::run(lowThreshold, 0.90f, lowCtx);
      RadioAMDetectorNode::run(highThreshold, 0.90f, highCtx);
    }
    addPredicate(rows, "AMDetector", "avc_diode_params_shift_charge_threshold",
                 lowThreshold.demod.am.avcEnv > highThreshold.demod.am.avcEnv,
                 lowThreshold.demod.am.avcEnv - highThreshold.demod.am.avcEnv,
                 "low-threshold minus high-threshold AVC");
  }

  {
    Radio1938 radio =
        makeNodeFixture(config, 0.0f, {PassId::Demod, PassId::DetectorAudio});
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
    Radio1938 radio =
        makeNodeFixture(config, 0.0f, {PassId::Demod, PassId::DetectorAudio});
    auto ripple = runDetectorRippleTrace(radio);
    addPredicate(rows, "AMDetector", "avc_rejects_audio_ripple",
                 ripple.avcRippleRms < 0.10 * std::max(ripple.audioRippleRms, 1e-12),
                 ripple.avcRippleRms / std::max(ripple.audioRippleRms, 1e-12),
                 "AVC ripple / detector-audio ripple");
  }

  {
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::ReceiverInputNetwork});
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
    Radio1938 radioNeg = makeNodeFixture(config, 0.0f, {PassId::ReceiverCircuit});
    Radio1938 radioPos = makeNodeFixture(config, 0.0f, {PassId::ReceiverCircuit});
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

    Radio1938 quiescent = makeNodeFixture(config, 0.0f, {PassId::ReceiverCircuit});
    quiescent.receiverCircuit.warmStartPending = false;
    quiescent.receiverCircuit.gridVoltage = 0.0f;
    quiescent.receiverCircuit.tubePlateVoltage =
        quiescent.receiverCircuit.tubeQuiescentPlateVolts;
    RadioReceiverCircuitNode::run(quiescent, 0.0f, ctx);
    addRange(rows, "ReceiverCircuit", "zero_grid_stays_near_quiescent_plate",
             quiescent.receiverCircuit.tubePlateVoltage,
             quiescent.receiverCircuit.tubeQuiescentPlateVolts -
                 quiescent.receiverCircuit.operatingPointToleranceVolts,
             quiescent.receiverCircuit.tubeQuiescentPlateVolts +
                 quiescent.receiverCircuit.operatingPointToleranceVolts,
             "quiescent grid bias should stay near solved operating point");
  }

  {
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::Power});
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

    Radio1938 radioDc = makeNodeFixture(config, 0.0f, {PassId::Power});
    RadioPowerNode::reset(radioDc);
    for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
      RadioPowerNode::run(radioDc, 10.0f, ctx);
    }
    addRange(rows, "Power", "driver_coupling_cap_blocks_dc",
             std::fabs(radioDc.power.gridVoltage), 0.0, 0.05,
             "steady-state driver grid AC volts");

    Radio1938 radioDrive = makeNodeFixture(config, 0.0f, {PassId::Power});
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
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::Tone});
    RadioToneNode::reset(radio);
    std::vector<float> toneOut(kNodeTestFrames, 0.0f);
    RadioSampleContext ctx{};
    for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
      toneOut[i] = RadioToneNode::run(radio, 0.0f, ctx);
    }
    addRange(rows, "Tone", "zero_input_stays_zero", rmsOf(toneOut), 0.0, 1e-7,
             "passive tone shaping");

    Radio1938 flat = makeNodeFixture(config, 0.0f, {PassId::Tone});
    Radio1938 bright = makeNodeFixture(config, 0.0f, {PassId::Tone});
    flat.tone.presenceHz = 0.0f;
    bright.tone.presenceHz = 2500.0f;
    bright.tone.presenceQ = 0.85f;
    bright.tone.presenceGainDb = 6.0f;
    RadioInitContext initCtx{};
    RadioToneNode::init(flat, initCtx);
    RadioToneNode::reset(flat);
    RadioToneNode::init(bright, initCtx);
    RadioToneNode::reset(bright);
    std::vector<float> flatOut(kNodeTestFrames, 0.0f);
    std::vector<float> brightOut(kNodeTestFrames, 0.0f);
    for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
      float t = static_cast<float>(i) / std::max(flat.sampleRate, 1.0f);
      float x = 0.2f * std::sin(kRadioTwoPi * 2500.0f * t);
      flatOut[i] = RadioToneNode::run(flat, x, ctx);
      brightOut[i] = RadioToneNode::run(bright, x, ctx);
    }
    addPredicate(rows, "Tone", "presence_peak_changes_high_band_gain",
                 rmsOf(brightOut, kNodeTestFrames / 2u) >
                     1.2 * std::max(rmsOf(flatOut, kNodeTestFrames / 2u), 1e-12),
                 rmsOf(brightOut, kNodeTestFrames / 2u) /
                     std::max(rmsOf(flatOut, kNodeTestFrames / 2u), 1e-12),
                 "bright / flat RMS at 2.5 kHz");
  }

  {
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::Noise});
    RadioNoiseNode::reset(radio);
    RadioSampleContext ctx{};
    ctx.derived = {};
    float y = RadioNoiseNode::run(radio, 0.125f, ctx);
    addRange(rows, "Noise", "zero_derived_noise_is_passthrough", y,
             0.125f - 1e-7, 0.125f + 1e-7, "no derived hiss/crackle/hum");

    Radio1938 noisy = makeNodeFixture(config, 0.0f, {PassId::Noise});
    RadioNoiseNode::reset(noisy);
    std::vector<float> noisyOut(kNodeTestFrames, 0.0f);
    for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
      RadioSampleContext noisyCtx{};
      noisyCtx.derived.noiseAmp = 0.05f;
      noisyOut[i] = RadioNoiseNode::run(noisy, 0.0f, noisyCtx);
    }
    addPredicate(rows, "Noise", "derived_noise_raises_output_rms",
                 rmsOf(noisyOut, kNodeTestFrames / 2u) > 1e-4,
                 rmsOf(noisyOut, kNodeTestFrames / 2u),
                 "output RMS with derived noise injection");
  }

  return rows;
}

std::vector<TestRow> runOutputPhysics(const HarnessConfig& config) {
  std::vector<TestRow> rows;

  {
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::Speaker});
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
    Radio1938 flat = makeNodeFixture(config, 0.0f, {PassId::Speaker});
    Radio1938 breakup = makeNodeFixture(config, 0.0f, {PassId::Speaker});
    breakup.speakerStage.speaker.upperBreakupHz = 2400.0f;
    breakup.speakerStage.speaker.upperBreakupQ = 0.90f;
    breakup.speakerStage.speaker.upperBreakupGainDb = 3.0f;
    auto flatOut = runSpeakerTone(flat, 2400.0f, 0.2f);
    auto breakupOut = runSpeakerTone(breakup, 2400.0f, 0.2f);
    addPredicate(rows, "Speaker", "upper_breakup_peak_changes_response",
                 rmsOf(breakupOut, kNodeTestFrames / 2u) >
                     1.10 * std::max(rmsOf(flatOut, kNodeTestFrames / 2u), 1e-12),
                 rmsOf(breakupOut, kNodeTestFrames / 2u) /
                     std::max(rmsOf(flatOut, kNodeTestFrames / 2u), 1e-12),
                 "breakup-enabled / flat RMS");
  }

  {
    Radio1938 flat = makeNodeFixture(config, 0.0f, {PassId::Speaker});
    Radio1938 hfLoss = makeNodeFixture(config, 0.0f, {PassId::Speaker});
    hfLoss.speakerStage.speaker.hfLossLpHz = 2200.0f;
    hfLoss.speakerStage.speaker.hfLossDepth = 0.85f;
    auto flatOut = runSpeakerTone(flat, 4000.0f, 0.2f);
    auto hfLossOut = runSpeakerTone(hfLoss, 4000.0f, 0.2f);
    addPredicate(rows, "Speaker", "hf_loss_depth_reduces_top_end",
                 rmsOf(hfLossOut, kNodeTestFrames / 2u) <
                     0.85 * std::max(rmsOf(flatOut, kNodeTestFrames / 2u), 1e-12),
                 rmsOf(hfLossOut, kNodeTestFrames / 2u) /
                     std::max(rmsOf(flatOut, kNodeTestFrames / 2u), 1e-12),
                 "hf-loss / flat RMS");
  }

  {
    Radio1938 symmetric = makeNodeFixture(config, 0.0f, {PassId::Speaker});
    Radio1938 asymmetric = makeNodeFixture(config, 0.0f, {PassId::Speaker});
    symmetric.speakerStage.speaker.limit = 0.12f;
    asymmetric.speakerStage.speaker.limit = 0.12f;
    asymmetric.speakerStage.speaker.asymBias = 0.60f;
    auto symmetricOut = runSpeakerTone(symmetric, 120.0f, 0.5f);
    auto asymmetricOut = runSpeakerTone(asymmetric, 120.0f, 0.5f);
    double symmetricSkew = std::fabs(
        positivePeakOf(symmetricOut, kNodeTestFrames / 2u) -
        negativePeakAbsOf(symmetricOut, kNodeTestFrames / 2u));
    double asymmetricSkew = std::fabs(
        positivePeakOf(asymmetricOut, kNodeTestFrames / 2u) -
        negativePeakAbsOf(asymmetricOut, kNodeTestFrames / 2u));
    addPredicate(rows, "Speaker", "asym_bias_skews_limit_symmetry",
                 asymmetricSkew > symmetricSkew + 1e-4, asymmetricSkew - symmetricSkew,
                 "asymmetric minus symmetric peak skew");
  }

  {
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::Cabinet});
    RadioCabinetNode::reset(radio);
    std::vector<float> out(kNodeTestFrames, 0.0f);
    RadioSampleContext ctx{};
    for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
      out[i] = RadioCabinetNode::run(radio, 0.0f, ctx);
    }
    addRange(rows, "Cabinet", "zero_input_stays_zero", rmsOf(out), 0.0, 1e-7,
             "passive cabinet coloration");

    Radio1938 flat = makeNodeFixture(config, 0.0f, {PassId::Cabinet});
    Radio1938 resonant = makeNodeFixture(config, 0.0f, {PassId::Cabinet});
    resonant.cabinet.panelHz = 180.0f;
    resonant.cabinet.panelGainDb = 5.0f;
    RadioInitContext initCtx{};
    RadioCabinetNode::init(flat, initCtx);
    RadioCabinetNode::reset(flat);
    RadioCabinetNode::init(resonant, initCtx);
    RadioCabinetNode::reset(resonant);
    std::vector<float> flatOut(kNodeTestFrames, 0.0f);
    std::vector<float> resonantOut(kNodeTestFrames, 0.0f);
    for (uint32_t i = 0; i < kNodeTestFrames; ++i) {
      float t = static_cast<float>(i) / std::max(flat.sampleRate, 1.0f);
      float x = 0.2f * std::sin(kRadioTwoPi * 180.0f * t);
      flatOut[i] = RadioCabinetNode::run(flat, x, ctx);
      resonantOut[i] = RadioCabinetNode::run(resonant, x, ctx);
    }
    addPredicate(rows, "Cabinet", "panel_resonance_changes_180hz_gain",
                 rmsOf(resonantOut, kNodeTestFrames / 2u) >
                     1.10 * std::max(rmsOf(flatOut, kNodeTestFrames / 2u), 1e-12),
                 rmsOf(resonantOut, kNodeTestFrames / 2u) /
                     std::max(rmsOf(flatOut, kNodeTestFrames / 2u), 1e-12),
                 "resonant / flat cabinet RMS");
  }

  {
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::OutputScale});
    RadioSampleContext ctx{};
    float scaled = RadioOutputScaleNode::run(
        radio, radio.output.digitalReferenceSpeakerVoltsPeak, ctx);
    addRange(rows, "OutputScale", "reference_maps_to_headroom_peak", scaled,
             (1.0 / 1.12) - 1e-6, (1.0 / 1.12) + 1e-6,
             "physical speaker volts to digital");

    Radio1938 makeup = makeNodeFixture(config, 0.0f, {PassId::OutputScale});
    makeup.output.digitalMakeupGain = 1.5f;
    float scaledMakeup = RadioOutputScaleNode::run(
        makeup, 0.25f * makeup.output.digitalReferenceSpeakerVoltsPeak, ctx);
    addRange(rows, "OutputScale", "digital_makeup_gain_is_linear", scaledMakeup,
             (0.25 / 1.12) * 1.5 - 1e-6, (0.25 / 1.12) * 1.5 + 1e-6,
             "post-reference digital makeup must be multiplicative");
  }

  {
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::FinalLimiter});
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

    Radio1938 clean = makeNodeFixture(config, 0.0f, {PassId::FinalLimiter});
    clean.finalLimiter.enabled = true;
    clean.finalLimiter.threshold = 0.8f;
    clean.finalLimiter.lookaheadMs = 0.0f;
    clean.finalLimiter.attackMs = 0.1f;
    clean.finalLimiter.releaseMs = 5.0f;
    RadioFinalLimiterNode::init(clean, initCtx);
    RadioFinalLimiterNode::reset(clean);
    float cleanOut = 0.0f;
    for (int i = 0; i < 32; ++i) {
      cleanOut = RadioFinalLimiterNode::run(clean, 0.1f, ctx);
    }
    addRange(rows, "FinalLimiter", "subthreshold_signal_is_unity_gain", cleanOut,
             0.1f - 1e-4, 0.1f + 1e-4,
             "below threshold limiter should be transparent");
  }

  {
    Radio1938 radio = makeNodeFixture(config, 0.0f, {PassId::OutputClip});
    RadioInitContext initCtx{};
    radio.globals.outputClipThreshold = 0.5f;
    RadioOutputClipNode::init(radio, initCtx);
    RadioOutputClipNode::reset(radio);
    RadioSampleContext ctx{};
    float out = RadioOutputClipNode::run(radio, 1.0f, ctx);
    addPredicate(rows, "OutputClip", "clip_threshold_marks_and_clamps",
                 radio.diagnostics.outputClipSamples > 0 && std::fabs(out) <= 0.5f,
                 out, "hard safety clamp");

    Radio1938 clean = makeNodeFixture(config, 0.0f, {PassId::OutputClip});
    clean.globals.outputClipThreshold = 0.5f;
    RadioOutputClipNode::init(clean, initCtx);
    RadioOutputClipNode::reset(clean);
    float cleanOut = RadioOutputClipNode::run(clean, 0.1f, ctx);
    addPredicate(rows, "OutputClip", "subthreshold_signal_does_not_mark_clip",
                 clean.diagnostics.outputClipSamples == 0 &&
                     std::fabs(cleanOut) < clean.globals.outputClipThreshold,
                 cleanOut, "below threshold clipper must stay inactive");
  }

  return rows;
}

void printConfig(const HarnessConfig& config) {
  std::cout << "[config]\n";
  std::cout << "key,value\n";
  std::cout << "sample_rate_hz," << config.sampleRate << "\n";
  std::cout << "bandwidth_hz," << config.bandwidthHz << "\n";
  std::cout << "carrier_rms_volts," << config.carrierRmsVolts << "\n";
  std::cout << "noise_weight," << config.noiseWeight << "\n";
  std::cout << "node," << config.node << "\n\n";
}

void printCoverage(const HarnessConfig& config, const std::vector<TestRow>& rows) {
  std::cout << "[coverage]\n";
  std::cout << "node,test_count,covered\n";
  for (const auto& [_, label] : kNodeLabels) {
    if (!wantsNode(config, label)) continue;
    size_t count = 0;
    for (const auto& row : rows) {
      if (row.node == label) ++count;
    }
    std::cout << label << "," << count << "," << (count > 0 ? 1 : 0) << "\n";
  }
  std::cout << "\n";
}

bool allRequestedNodesCovered(const HarnessConfig& config,
                              const std::vector<TestRow>& rows) {
  for (const auto& [_, label] : kNodeLabels) {
    if (!wantsNode(config, label)) continue;
    bool covered = false;
    for (const auto& row : rows) {
      if (row.node == label) {
        covered = true;
        break;
      }
    }
    if (!covered) return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    HarnessConfig config = parseArgs(argc, argv);
    if (!isKnownNode(config.node)) {
      fail("unknown --node value: " + config.node);
    }
    std::cout << std::setprecision(9);
    printConfig(config);

    bool ok = true;
    std::vector<TestRow> allRows;
    if (wantsSection(config, "control")) {
      auto rows = filterRowsForNode(config, runControlPhysics(config));
      printRows("control", rows);
      ok = ok && allPassed(rows);
      allRows.insert(allRows.end(), rows.begin(), rows.end());
    }
    if (wantsSection(config, "rf")) {
      auto rows = filterRowsForNode(config, runRfPhysics(config));
      printRows("rf", rows);
      ok = ok && allPassed(rows);
      allRows.insert(allRows.end(), rows.begin(), rows.end());
    }
    if (wantsSection(config, "detector")) {
      auto rows = filterRowsForNode(config, runDetectorPhysics(config));
      printRows("detector", rows);
      ok = ok && allPassed(rows);
      allRows.insert(allRows.end(), rows.begin(), rows.end());
    }
    if (wantsSection(config, "audio")) {
      auto rows = filterRowsForNode(config, runAudioPhysics(config));
      printRows("audio", rows);
      ok = ok && allPassed(rows);
      allRows.insert(allRows.end(), rows.begin(), rows.end());
    }
    if (wantsSection(config, "output")) {
      auto rows = filterRowsForNode(config, runOutputPhysics(config));
      printRows("output", rows);
      ok = ok && allPassed(rows);
      allRows.insert(allRows.end(), rows.begin(), rows.end());
    }
    printCoverage(config, allRows);
    ok = ok && allRequestedNodesCovered(config, allRows);
    return ok ? 0 : 2;
  } catch (const std::exception& ex) {
    std::cerr << "radio_node_physics: " << ex.what() << "\n";
    return 1;
  }
}
