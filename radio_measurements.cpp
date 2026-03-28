#include "radio.h"

#include <algorithm>
#include <array>
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
#include <vector>

namespace {

constexpr float kDefaultSampleRate = 48000.0f;
constexpr float kDefaultBandwidthHz = 5000.0f;
constexpr float kDefaultCarrierRmsVolts = 0.12f;
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

struct HarnessConfig {
  float sampleRate = kDefaultSampleRate;
  float bandwidthHz = kDefaultBandwidthHz;
  float carrierRmsVolts = kDefaultCarrierRmsVolts;
  float modulationIndex = kDefaultModulationIndex;
  float noiseWeight = kDefaultNoiseWeight;
  float noiseOnlyWeight = kDefaultNoiseOnlyWeight;
  std::string settingsPath;
  std::string presetName;
  std::string section = "all";
};

// --- Target ranges for harness validation (user-specified anchors) ---
constexpr float kTargetAudioLow3dBMin = 60.0f;
constexpr float kTargetAudioLow3dBMax = 200.0f;
constexpr float kTargetAudioHigh3dBMin = 2500.0f;
constexpr float kTargetAudioHigh3dBMax = 5500.0f;
constexpr float kTargetDetectorTauUsMin = 35.0f;
constexpr float kTargetDetectorTauUsMax = 55.0f;
constexpr float kTargetAvcTauMsMin = 60.0f;
constexpr float kTargetAvcTauMsMax = 90.0f;
// IMD: lower is better. Expect <= -25 dB (prefer ~-30 dB)
constexpr float kTargetImdDbMin = -100.0f;
constexpr float kTargetImdDbMax = -25.0f;
constexpr float kTargetSinadNominalDbMin = 35.0f;
constexpr float kTargetSinadNominalDbMax = 100.0f;
constexpr float kTargetSpeakerReferenceRatioMin = 0.90f;
constexpr float kTargetSpeakerReferenceRatioMax = 1.10f;
// Prefer max digital output < ~0.95
constexpr float kTargetMaxDigitalOutputMin = 0.0f;
constexpr float kTargetMaxDigitalOutputMax = 0.95f;
// IF center and nominal output power
constexpr float kTargetIfCenterMinHz = 469000.0f;
constexpr float kTargetIfCenterMaxHz = 471000.0f;
constexpr float kTargetNominalOutputPowerMinW = 12.0f;
constexpr float kTargetNominalOutputPowerMaxW = 18.0f;
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

[[noreturn]] void fail(const std::string& message) {
  throw std::runtime_error(message);
}

double dbFromRatio(double ratio) {
  return (ratio > 1e-20) ? 20.0 * std::log10(ratio) : -300.0;
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

double avcTauSeconds(const Radio1938& radio) {
  return std::max<double>(radio.demod.am.avcDischargeResistanceOhms, 1e-6) *
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

void printUsage() {
  std::cout
      << "radio_measurements [options]\n"
      << "  --sample-rate <hz>\n"
      << "  --bw <hz>\n"
      << "  --carrier-rms <volts>\n"
      << "  --mod-index <value>\n"
      << "  --noise <value>\n"
      << "  --noise-only <value>\n"
      << "  --radio-settings <path>\n"
      << "  --preset <name>\n"
      << "  --section <all|sweep|envelope|levels|imd|overdrive|sinad|weak|transient|reference|noise|spectrum|am_validate>\n";
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
    } else if (arg == "--mod-index") {
      config.modulationIndex = parseFloatArg(argv, i, argc, "--mod-index");
    } else if (arg == "--noise") {
      config.noiseWeight = parseFloatArg(argv, i, argc, "--noise");
    } else if (arg == "--noise-only") {
      config.noiseOnlyWeight = parseFloatArg(argv, i, argc, "--noise-only");
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

void warmCarrier(Radio1938& radio, float carrierRmsVolts) {
  radio.warmInputCarrier(carrierRmsVolts, kWarmupFrames);
}

Radio1938 makeRadio(const HarnessConfig& config, float noiseWeight,
                    bool warmWithCarrier) {
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
  result.radio.processAmAudio(program.data(), result.output.data(),
                              static_cast<uint32_t>(program.size()),
                              config.carrierRmsVolts, modulationIndex);
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

EnvelopeTraceResult runAmProgramWithEnvelopeTrace(const HarnessConfig& config,
                                                  const std::vector<float>& program,
                                                  float modulationIndex,
                                                  float noiseWeight) {
  EnvelopeTraceResult result{makeRadio(config, noiseWeight, true), {}, {}, {}, {}};
  result.output.resize(program.size(), 0.0f);
  result.detectorNode.resize(program.size(), 0.0f);
  result.audioEnv.resize(program.size(), 0.0f);
  result.avcEnv.resize(program.size(), 0.0f);

  float safeSampleRate = std::max(config.sampleRate, 1.0f);
  float carrierHz = result.radio.resolvedInputCarrierHz();
  float carrierStep = kRadioTwoPi * (carrierHz / safeSampleRate);
  float carrierPeak =
      std::sqrt(2.0f) * std::max(config.carrierRmsVolts, 0.0f);
  float phase = result.radio.iqInput.iqPhase;
  float normScale = amProgramNormScale(program);
  std::array<float, 1> sample{};
  for (size_t i = 0; i < program.size(); ++i) {
    float envelopeFactor =
        std::max(0.0f, 1.0f + modulationIndex * program[i] * normScale);
    sample[0] = carrierPeak * envelopeFactor * std::cos(phase);
    phase += carrierStep;
    if (phase >= kRadioTwoPi) phase -= kRadioTwoPi;
    result.radio.processIfReal(sample.data(), 1u);
    result.output[i] = sample[0];
    result.detectorNode[i] = result.radio.demod.am.detectorNode;
    result.audioEnv[i] = result.radio.detectorAudio.audioEnv;
    result.avcEnv[i] = result.radio.demod.am.avcEnv;
  }
  result.radio.iqInput.iqPhase = phase;
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
  std::cout << "[config]\n";
  std::cout << "key,value\n";
  std::cout << "sample_rate_hz," << config.sampleRate << "\n";
  std::cout << "bandwidth_hz," << config.bandwidthHz << "\n";
  std::cout << "carrier_rms_volts," << config.carrierRmsVolts << "\n";
  std::cout << "modulation_index," << config.modulationIndex << "\n";
  std::cout << "noise_weight," << config.noiseWeight << "\n";
  std::cout << "noise_only_weight," << config.noiseOnlyWeight << "\n\n";
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

ToneMetrics measureNominalSinad(const HarnessConfig& config) {
  HarnessConfig nominalConfig = config;
  nominalConfig.carrierRmsVolts = kDefaultCarrierRmsVolts;
  auto input =
      makeSine(config.sampleRate, kSteadyTestFrames, 1000.0f,
               kNominalProgramPeak);
  RunResult run =
      runAmProgram(nominalConfig, input, nominalHarnessModulationIndex(config),
                   std::max(config.noiseWeight, config.noiseOnlyWeight));
  return measureToneMetrics(run.output, config.sampleRate, 1000.0f,
                            static_cast<size_t>(config.sampleRate * 0.20f));
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
  std::cout << "driver_grid_rms," << run.radio.calibration.driverGridVolts.rms
            << "\n";
  std::cout << "output_primary_rms,"
            << run.radio.calibration.outputPrimaryVolts.rms << "\n";
  std::cout << "speaker_secondary_rms,"
            << run.radio.calibration.speakerSecondaryVolts.rms << "\n";
  std::cout << "speaker_secondary_peak,"
            << run.radio.calibration.maxSpeakerSecondaryVolts << "\n";
  std::cout << "speaker_reference_ratio,"
            << run.radio.calibration.maxSpeakerReferenceRatio << "\n";
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
  auto input = makeSine(config.sampleRate, kSteadyTestFrames, 1000.0f,
                        kOverdriveProgramPeak);
  constexpr std::array<float, 8> kModIndices = {0.10f, 0.25f, 0.50f, 0.75f,
                                                0.90f, 1.00f, 1.10f, 1.20f};
  for (float modIndex : kModIndices) {
    RunResult run = runAmProgram(config, input, modIndex, config.noiseWeight);
    SignalStats stats =
        measureSignalStats(run.output, static_cast<size_t>(config.sampleRate * 0.20f));
    std::vector<double> spectrum =
        computeSpectrumPeakAmplitudes(run.output, config.sampleRate);
    double h1 = peakAmplitudeAtHz(spectrum, config.sampleRate, 1000.0f);
    double h2 = peakAmplitudeAtHz(spectrum, config.sampleRate, 2000.0f);
    double h3 = peakAmplitudeAtHz(spectrum, config.sampleRate, 3000.0f);
    double h4 = peakAmplitudeAtHz(spectrum, config.sampleRate, 4000.0f);
    double h5 = peakAmplitudeAtHz(spectrum, config.sampleRate, 5000.0f);
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
              << run.radio.calibration.maxSpeakerReferenceRatio << ","
              << (run.radio.diagnostics.powerClip ? 1 : 0) << ","
              << (run.radio.diagnostics.speakerClip ? 1 : 0) << ","
              << (run.radio.diagnostics.outputClip ? 1 : 0) << ","
              << (run.radio.diagnostics.finalLimiterActive ? 1 : 0) << ","
              << (run.radio.calibration.validationDigitalClip ? 1 : 0) << ","
              << (run.radio.calibration.validationFailed ? 1 : 0) << "\n";
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
              << "," << run.radio.calibration.maxSpeakerReferenceRatio << ","
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
  constexpr float kBurstHz = 10.0f;
  auto rf = makeCarrierBurstRf(config.sampleRate, kSteadyTestFrames, carrierHz,
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

void printReferenceRow(const char* metric,
                       double measured,
                       double low,
                       double high) {
  bool inBand = (measured >= low && measured <= high);
  std::cout << metric << "," << measured << "," << low << "," << high << ","
            << (inBand ? 1 : 0) << "\n";
}

void runReference(const HarnessConfig& config) {
  std::cout << "[reference]\n";
  std::cout << "metric,measured,low,high,in_band\n";
  SweepSummary sweep = summarizeSweep(measureSweepPoints(config));
  EdgeMetrics envelope =
      measureEnvelopeMetrics(
          runAmProgramWithEnvelopeTrace(
              config, makeSquare(config.sampleRate, kEnvelopeTestFrames, 30.0f, 0.85f),
              config.modulationIndex, config.noiseWeight)
              .audioEnv,
          config.sampleRate, 30.0f);
  ToneMetrics sinad = measureNominalSinad(config);
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
  double detectorTauUs = 1e6 * effectiveDetectorAudioTauSeconds(radio);
  double avcTauMs = 1e3 * avcTauSeconds(radio);

  // Working bands only. The frequency-domain limits are still coarse. The
  // detector / AVC timing rows below are based on named RC state and service
  // sideband constraints, not on arbitrary output-envelope taste metrics.
  printReferenceRow("audio_low_3db_hz", sweep.low3dBHz,
                    kTargetAudioLow3dBMin, kTargetAudioLow3dBMax);
  printReferenceRow("audio_high_3db_hz", sweep.high3dBHz,
                    kTargetAudioHigh3dBMin, kTargetAudioHigh3dBMax);
  printReferenceRow("imd_db", imdDb, kTargetImdDbMin, kTargetImdDbMax);
  printReferenceRow("detector_tau_us", detectorTauUs,
                    kTargetDetectorTauUsMin, kTargetDetectorTauUsMax);
  printReferenceRow("avc_tau_ms", avcTauMs, kTargetAvcTauMsMin,
                    kTargetAvcTauMsMax);
  printReferenceRow("avc_to_detector_tau_ratio",
                    avcTauMs / std::max(detectorTauUs * 1e-3, 1e-9), 50.0,
                    1e9);
  printReferenceRow("envelope_rise_ms_observed", envelope.rise10To90Ms, 0.0,
                    10.0);
  printReferenceRow("envelope_fall_ms_observed", envelope.fall90To10Ms, 0.0,
                    10.0);
  printReferenceRow("sinad_nominal_db", sinad.sinadDb,
                    kTargetSinadNominalDbMin, kTargetSinadNominalDbMax);
  // Speaker/reference and digital output targets (measured from nominal run)
  double speakerRefRatio = nominalRun.radio.calibration.maxSpeakerReferenceRatio;
  double maxDigitalOutput = nominalRun.radio.calibration.maxDigitalOutput;
  printReferenceRow("speaker_reference_ratio", speakerRefRatio,
                    kTargetSpeakerReferenceRatioMin,
                    kTargetSpeakerReferenceRatioMax);
  printReferenceRow("max_digital_output", maxDigitalOutput,
                    kTargetMaxDigitalOutputMin, kTargetMaxDigitalOutputMax);
  // Diagnostic flags expected to be zero in nominal bench
  printReferenceRow("power_clip", nominalRun.radio.diagnostics.powerClip ? 1 : 0,
                    kTargetFlagZero, kTargetFlagZero);
  printReferenceRow("speaker_clip", nominalRun.radio.diagnostics.speakerClip ? 1 : 0,
                    kTargetFlagZero, kTargetFlagZero);
  printReferenceRow("output_clip", nominalRun.radio.diagnostics.outputClip ? 1 : 0,
                    kTargetFlagZero, kTargetFlagZero);
  printReferenceRow("final_limiter_active",
                    nominalRun.radio.diagnostics.finalLimiterActive ? 1 : 0,
                    kTargetFlagZero, kTargetFlagZero);
  // IF center and nominal output power anchor
  printReferenceRow("if_center_hz", radio.ifStrip.ifCenterHz,
                    kTargetIfCenterMinHz, kTargetIfCenterMaxHz);
  printReferenceRow("nominal_output_power_watts",
                    radio.power.nominalOutputPowerWatts,
                    kTargetNominalOutputPowerMinW,
                    kTargetNominalOutputPowerMaxW);
  std::cout << "\n";
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

  // Program (audio) used for AM excitation
  auto program =
      makeSine(config.sampleRate, kSteadyTestFrames, 1000.0f, kSweepProgramPeak);
  float modulationIndex = std::min(config.modulationIndex, 0.999f);
  float effectiveNoiseWeight =
      std::max(config.noiseWeight, config.noiseOnlyWeight);
  // Run trace to collect detector/audio/env data
  EnvelopeTraceResult trace = runAmProgramWithEnvelopeTrace(
      config, program, modulationIndex, effectiveNoiseWeight);
  RunResult directRun =
      runAmProgram(config, program, modulationIndex, effectiveNoiseWeight);

  double maxOutputAbsDiff = 0.0;
  if (effectiveNoiseWeight <= 0.0f) {
    for (size_t i = 0; i < directRun.output.size() && i < trace.output.size();
         ++i) {
      maxOutputAbsDiff =
          std::max(maxOutputAbsDiff,
                   std::fabs(static_cast<double>(directRun.output[i]) -
                             static_cast<double>(trace.output[i])));
    }
    std::cout << "am_output_max_abs_diff," << maxOutputAbsDiff << "\n";
    if (maxOutputAbsDiff > 1e-4) {
      fail("AM output mismatch between processAmAudio and envelope trace: " +
           std::to_string(maxOutputAbsDiff));
    }
  } else {
    std::cout << "am_output_compare_skipped,1\n";
  }
  std::cout << "am_output_max_abs_diff," << maxOutputAbsDiff << "\n";

  // Reconstruct RF samples used for the run (same envelope & carrier)
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
  double effectiveModulationDepth = modulationIndex * programPeakAfterNorm;
  for (size_t i = 0; i < program.size(); ++i) {
    float t = static_cast<float>(i) / safeSampleRate;
    float sampleVal = program[i] * normScale;
    float envelopeFactor = std::max(0.0f, 1.0f + modulationIndex * sampleVal);
    rfSamples[i] = carrierPeak * envelopeFactor * std::cos(kRadioTwoPi * carrierHz * t);
  }

  // RF-domain carrier and sideband checks.
  double carrierRms = measureToneRms(rfSamples, config.sampleRate, carrierHz, 0);
  double sideUpperRms =
      measureToneRms(rfSamples, config.sampleRate, carrierHz + 1000.0f, 0);
  double sideLowerRms =
      measureToneRms(rfSamples, config.sampleRate, carrierHz - 1000.0f, 0);
  double expectedSideRms = (effectiveModulationDepth * 0.5) * carrierRms;
  double sideAvgRms = 0.5 * (sideUpperRms + sideLowerRms);
  double sidebandRatio =
      sideAvgRms / std::max(expectedSideRms, 1e-20);
  bool sidebandRatioInBand =
      sidebandRatio >= kAmSidebandRatioMin &&
      sidebandRatio <= kAmSidebandRatioMax;

  std::cout << "carrier_rms," << carrierRms << "\n";
  std::cout << "sideband_upper_rms," << sideUpperRms << "\n";
  std::cout << "sideband_lower_rms," << sideLowerRms << "\n";
  std::cout << "sideband_expected_per_side_rms," << expectedSideRms << "\n";
  std::cout << "sideband_avg_ratio_vs_expected," << sidebandRatio << "\n";
  std::cout << "sideband_ratio_in_band," << (sidebandRatioInBand ? 1 : 0)
            << "\n";
  if (!sidebandRatioInBand) {
    fail("AM sideband ratio out of range: measured " +
         std::to_string(sidebandRatio) + ", expected [" +
         std::to_string(kAmSidebandRatioMin) + ", " +
         std::to_string(kAmSidebandRatioMax) + "]");
  }

  // Detector-domain analysis
  std::vector<double> detSpec = computeSpectrumPeakAmplitudes(trace.detectorNode, config.sampleRate);
  double detDc = (detSpec.size() > 0) ? detSpec[0] : 0.0;
  double detTone = peakAmplitudeAtHz(detSpec, config.sampleRate, 1000.0f);
  std::cout << "detector_dc," << detDc << "\n";
  std::cout << "detector_tone_1khz," << detTone << "\n";
  std::cout << "detector_tone_to_dc_ratio," << (detTone / std::max(detDc, 1e-20)) << "\n";

    // Envelope-derived modulation index (from detector audioEnv)
    double envMax = -1e300, envMin = 1e300;
    size_t start = program.size() / 4;
    for (size_t i = start; i < trace.audioEnv.size(); ++i) {
      envMax = std::max(envMax, static_cast<double>(trace.audioEnv[i]));
      envMin = std::min(envMin, static_cast<double>(trace.audioEnv[i]));
    }
    double m_measured = 0.0;
    if (envMax + envMin > 1e-12) m_measured = (envMax - envMin) / (envMax + envMin);
    std::cout << "mod_index_config," << modulationIndex << "\n";
    std::cout << "mod_index_expected_effective," << effectiveModulationDepth << "\n";
    std::cout << "mod_index_measured_env," << m_measured << "\n";
    bool modIndexInBand =
        std::fabs(m_measured - effectiveModulationDepth) <=
        kAmModIndexTolerance;
    std::cout << "mod_index_in_band," << (modIndexInBand ? 1 : 0) << "\n";
    if (!modIndexInBand) {
      fail("AM modulation index out of range: measured " +
           std::to_string(m_measured) + ", expected around " +
           std::to_string(effectiveModulationDepth));
    }

    // Tone metrics and SINAD at detector and final output
    ToneMetrics detToneMetrics = measureToneMetrics(trace.detectorNode, config.sampleRate, 1000.0f, start);
    ToneMetrics outToneMetrics = measureToneMetrics(trace.output, config.sampleRate, 1000.0f, start);
    std::cout << "detector_sinad_db," << detToneMetrics.sinadDb << "\n";
    std::cout << "output_sinad_db," << outToneMetrics.sinadDb << "\n";

    // Speaker reference & digital output anchors (from radio calibration)
    double speakerRef = trace.radio.calibration.maxSpeakerReferenceRatio;
    double maxDigital = trace.radio.calibration.maxDigitalOutput;
    std::cout << "speaker_reference_ratio," << speakerRef << "\n";
    std::cout << "max_digital_output," << maxDigital << "\n";
    std::cout << "if_center_hz," << trace.radio.ifStrip.ifCenterHz << "\n";
    std::cout << "nominal_output_power_watts," << trace.radio.power.nominalOutputPowerWatts << "\n";
    std::cout << "\n";
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
    if (wantsSection(config, "transient")) runTransient(config);
    if (wantsSection(config, "reference")) runReference(config);
    if (wantsSection(config, "noise")) runNoise(config);
    if (wantsSection(config, "spectrum")) runSpectrum(config);
    if (wantsSection(config, "am_validate")) runAmValidate(config);
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "radio_measurements: " << ex.what() << "\n";
    return 1;
  }
}
