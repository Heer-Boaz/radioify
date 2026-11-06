#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "audiofilter/math/signal_math.h"
#include "audiofilter/models/tube_models.h"
#include "radio.h"

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kFrames = 8192;

// Former production algorithm, retained only as a measured comparison.
struct LegacyOversampler {
  float previous = 0;
  Biquad a, b;
  explicit LegacyOversampler(float fs) {
    a.setLowpass(2 * fs, .45f * fs, .7071067812f);
    b = a;
  }
  template <typename Process>
  float process(float x, Process&& fn) {
    const float first = fn(.5f * (previous + x));
    const float second = fn(x);
    b.process(a.process(first));
    previous = x;
    return b.process(a.process(second));
  }
};

void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "radio_oversampling_tests: " << message << '\n';
    std::exit(1);
  }
}

std::vector<std::complex<double>> spectrum(const std::vector<float>& signal) {
  const size_t count = signal.size();
  std::vector<std::complex<double>> bins(signal.begin(), signal.end());
  for (size_t i = 1, j = 0; i < count; ++i) {
    size_t bit = count >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j)
      std::swap(bins[i], bins[j]);
  }
  for (size_t width = 2; width <= count; width *= 2) {
    const auto rotation = std::polar(1.0, -2 * kPi / width);
    for (size_t start = 0; start < count; start += width) {
      std::complex<double> phase{1, 0};
      for (size_t i = 0; i < width / 2; ++i) {
        const auto even = bins[start + i];
        const auto odd = phase * bins[start + i + width / 2];
        bins[start + i] = even + odd;
        bins[start + i + width / 2] = even - odd;
        phase *= rotation;
      }
    }
  }
  for (auto& bin : bins)
    bin *= 2.0 / count;
  return bins;
}

double aliasDb(const std::vector<float>& signal, int toneBin,
               double highestFraction = .45) {
  const auto bins = spectrum(signal);
  double aliases = 0;
  for (size_t bin = 1; bin < static_cast<size_t>(bins.size() * highestFraction); ++bin)
    if (bin % toneBin != 0)
      aliases += std::norm(bins[bin]);
  return 10 * std::log10(std::max(aliases, 1e-30) / std::norm(bins[toneBin]));
}

void testStreamingAndRates() {
  for (int factor : {1, 2, 4, 8}) {
    for (float passband : {.05f, .45f, .49f}) {
      Oversampler filter;
      filter.configure(static_cast<float>(factor), passband);
      int calls = 0;
      float dc = 0;
      for (int i = 0; i < 4096; ++i)
        dc = filter.process(.3f, [&](float x) {
          ++calls;
          return x;
        });
      check(calls == 4096 * factor, "callback clock disagrees with configured factor");
      check(std::fabs(dc - .3f) < 1e-6f, "resampler changed DC gain");
      for (int bin : {23, static_cast<int>(.98 * passband * kFrames)}) {
        filter.reset();
        std::vector<float> output(kFrames);
        for (int i = -kFrames; i < kFrames; ++i) {
          const float x = static_cast<float>(std::sin(2 * kPi * bin * i / kFrames));
          const float y = filter.process(x, [](float v) { return v; });
          if (factor == 1)
            check(x == y, "1x must be exact, delay-free bypass");
          if (i >= 0)
            output[i] = y;
        }
        const auto bins = spectrum(output);
        check(std::fabs(std::abs(bins[bin]) - 1) < 3e-5,
              "resampler changed passband gain");
      }
      Oversampler cleared;
      cleared.configure(static_cast<float>(factor), passband);
      filter.reset();
      for (int i = 0; i < 300; ++i) {
        const float x = std::sin(.137f * i);
        check(cleared.process(x, [](float v) { return v * v; }) ==
                  filter.process(x, [](float v) { return v * v; }),
              "reset retained resampling history");
      }
    }
  }
  for (float invalid : {0.0f, 1.5f, 3.0f, 16.0f}) {
    bool rejected = false;
    try {
      Oversampler filter;
      filter.configure(invalid, .45f);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "unsupported oversampling factor accepted");
  }
}

void reportResamplerDelay() {
  constexpr int bin = 171;  // Approximately 1 kHz at 48 kHz.
  for (int factor : {1, 2, 4, 8}) {
    Oversampler filter;
    filter.configure(static_cast<float>(factor), .45f);
    std::vector<float> impulse(kFrames);
    for (int i = 0; i < kFrames; ++i)
      impulse[i] = filter.process(i == 0 ? 1.0f : 0, [](float v) { return v; });
    const auto response = spectrum(impulse);
    const double delay =
        -std::arg(response[bin + 1] * std::conj(response[bin - 1])) / (4 * kPi / kFrames);
    std::cout << "resampler_delay," << factor << ",at_hz=" << bin * 48000.0 / kFrames
              << ",samples=" << delay << '\n';
    check(delay >= 0 && delay < 16, "unexpected resampler delay in radio passband");
  }
}

void testPhysicalFilterClock() {
  for (float fs : {44100.0f, 48000.0f, 96000.0f}) {
    for (int factor : {1, 2, 4, 8}) {
      Radio1938 radio;
      radio.globals.oversampleFactor = static_cast<float>(factor);
      radio.init(1, fs, 5000, 0);
      check(radio.power.internalSampleRate == fs * factor,
            "power-model integration clock disagrees with oversampling");
      auto& s = radio.speakerStage.speaker;
      s.suspensionGainDb = s.coneBodyGainDb = s.upperBreakupGainDb = s.coneDipGainDb = 0;
      s.coneMassTolerance = s.suspensionComplianceTolerance = s.voiceCoilTolerance = 0;
      s.hfLossDepth = s.limit = 0;
      s.filterQ = .7071067812f;
      constexpr int bin = 371;
      s.topLpHz = fs * bin / kFrames;
      radio.speakerStage.drive = 1;
      RadioInitContext init{};
      RadioSpeakerNode::init(radio, init);
      RadioSpeakerNode::reset(radio);
      RadioSampleContext ctx{};
      std::vector<float> output(kFrames);
      for (int i = -kFrames; i < kFrames; ++i) {
        const float x = static_cast<float>(std::sin(2 * kPi * bin * i / kFrames));
        const float y = RadioSpeakerNode::run(radio, x, ctx);
        if (i >= 0)
          output[i] = y;
      }
      const double amplitude = std::abs(spectrum(output)[bin]);
      check(std::fabs(amplitude - std::sqrt(.5)) < 3e-4,
            "physical lowpass corner moved when oversampling factor changed");
    }
  }
}

void testImageFilter() {
  for (double pass : {.05, .1, .2}) {
    IQFirLowpass filter;
    filter.configure(pass, pass + .1);
    std::vector<float> impulse(kFrames);
    for (int i = 0; i < kFrames; ++i) {
      const auto y = filter.process(i == 0 ? 1.0f : 0, i == 0 ? -.5f : 0);
      impulse[i] = y[0];
      check(y[1] == -.5f * y[0], "IQ image filter mixed or mismatched channels");
    }
    const auto response = spectrum(impulse);
    for (int bin = 0; bin <= kFrames / 2; ++bin) {
      const double gain = std::abs(response[bin]) * (kFrames / 2);
      const double frequency = static_cast<double>(bin) / kFrames;
      if (frequency <= pass)
        check(std::fabs(gain - 1) < 3e-5, "image filter altered its passband");
      if (frequency >= pass + .1)
        check(gain < 3e-5, "image filter failed stopband rejection");
    }
    check(filter.delaySamples() < 64, "image filter latency unexpectedly large");
  }
}

void testLimiterImmediatePeak() {
  for (int factor : {1, 2, 4, 8}) {
    Radio1938 radio;
    radio.sampleRate = 48000;
    radio.globals.oversampleFactor = static_cast<float>(factor);
    radio.finalLimiter.enabled = true;
    radio.finalLimiter.lookaheadMs = 0;
    radio.finalLimiter.threshold = .6f;
    RadioInitContext init{};
    RadioFinalLimiterNode::init(radio, init);
    RadioSampleContext ctx{};
    for (float polarity : {-1.0f, 1.0f}) {
      RadioFinalLimiterNode::reset(radio);
      const float output = RadioFinalLimiterNode::run(radio, polarity * 1.8f, ctx);
      check(std::fabs(output) <= .600001f,
            "delayed interpolation missed an immediate limiter peak");
    }
  }
}

void testRfImageRejection() {
  for (float fs : {8000.0f, 16000.0f, 44100.0f, 48000.0f, 96000.0f}) {
    for (float bandwidth : {4000.0f, 8000.0f}) {
      Radio1938 radio;
      radio.init(1, fs, bandwidth, 0);
      // Isolate representation conversion from physical RF/IF selectivity.
      radio.frontEnd.rfGain = radio.ifStrip.stageGain = radio.mixer.conversionGain = 1;
      radio.ifStrip.transformerCount = 0;  // Isolate representation conversion.
      const int bin = static_cast<int>(.9 * radio.tuning.tunedBw / fs * kFrames);
      std::vector<float> output(kFrames);
      float c = 1, s = 0;
      RadioSampleContext ctx{};
      for (int i = -kFrames; i < kFrames; ++i) {
        const float envelope =
            1 + .4f * static_cast<float>(std::sin(2 * kPi * bin * i / kFrames));
        const float rf = envelope * c;
        ctx.signal.setRealRf(rf);
        RadioIFStripNode::run(radio, rf, ctx);
        advanceNormalizedOscillator(radio.ifStrip.sourceDownmixStepCos,
                                    radio.ifStrip.sourceDownmixStepSin, c, s);
        if (i >= 0)
          output[i] = ctx.signal.detectorInputI;
      }
      const auto bins = spectrum(output);
      double unwanted = 0;
      for (int i = 1; i <= kFrames / 2; ++i)
        if (i != bin)
          unwanted += std::norm(bins[i]);
      const double alias = 10 * std::log10(unwanted / std::norm(bins[bin]));
      std::cout << "rf_image," << fs << ',' << bandwidth << ",residual_db=" << alias
                << ",delay_samples=" << radio.ifStrip.sourceEnvelope.delaySamples()
                << '\n';
      check(std::fabs(std::abs(bins[bin]) - .4) < 2e-5,
            "real RF extraction changed wanted sideband amplitude");
      check(alias < -85, "real RF extraction leaked the mirrored channel");
    }
  }
}

void reportLegacyPolynomial() {
  constexpr int toneBin = kFrames * 7 / 32;
  constexpr int aliasBin = kFrames - 3 * toneBin;
  LegacyOversampler legacy(48000);
  std::vector<float> signal(kFrames);
  for (int i = -kFrames; i < kFrames; ++i) {
    const float x = static_cast<float>(std::sin(2 * kPi * toneBin * i / kFrames));
    const float y = legacy.process(x, [](float v) { return v * v * v; });
    if (i >= 0)
      signal[i] = y;
  }
  const auto bins = spectrum(signal);
  std::cout << "legacy,cubic_alias_db,"
            << 20 * std::log10(std::abs(bins[aliasBin] / bins[toneBin])) << '\n';
  Oversampler oversampler;
  oversampler.configure(2, .45f);
  std::vector<float> up(kFrames * 2);
  for (int i = -kFrames; i < kFrames; ++i) {
    const float x = static_cast<float>(std::sin(2 * kPi * toneBin * i / kFrames));
    const auto& pair = oversampler.upsample(x);
    if (i >= 0) {
      up[2 * i] = pair[0];
      up[2 * i + 1] = pair[1];
    }
  }
  const auto upBins = spectrum(up);
  std::cout << "new,interpolation_image_db,"
            << 20 * std::log10(std::abs(upBins[kFrames - toneBin] / upBins[toneBin]))
            << '\n';
  oversampler.reset();
  for (int i = -kFrames; i < kFrames; ++i) {
    const float x = static_cast<float>(std::sin(2 * kPi * toneBin * i / kFrames));
    const float y = oversampler.process(x, [](float v) { return v * v * v; });
    if (i >= 0)
      signal[i] = y;
  }
  const auto newBins = spectrum(signal);
  std::cout << "new,cubic_alias_db,"
            << 20 * std::log10(std::abs(newBins[aliasBin] / newBins[toneBin])) << '\n';
  check(std::abs(newBins[aliasBin] / newBins[toneBin]) < 1e-5,
        "decimator did not reject the cubic's out-of-band harmonic");
}

std::vector<float> receiverTone(RadioReceiverProfile profile, float fs, int factor,
                                float drive, int toneBin, int oversampling = 2) {
  Radio1938 radio;
  radio.applyReceiverProfile(profile);
  radio.sampleRate = fs * factor;
  radio.globals.oversampleFactor = static_cast<float>(std::max(oversampling, 1));
  radio.noiseConfig.humHzDefault = 0;  // DC bench supply for alias isolation
  RadioInitContext init{};
  RadioReceiverCircuitNode::init(radio, init);
  RadioReceiverCircuitNode::reset(radio);
  radio.receiverCircuit.warmStartPending = false;
  RadioSampleContext sample{};
  LegacyOversampler legacy(fs * factor);
  std::vector<float> signal(kFrames * factor);
  for (int i = -kFrames * factor; i < kFrames * factor; ++i) {
    radio.receiverCircuit.gridVoltage =
        drive * static_cast<float>(std::sin(2 * kPi * toneBin * i / (kFrames * factor)));
    const float y =
        oversampling ? RadioReceiverCircuitNode::run(radio, 0, sample)
                     : legacy.process(radio.receiverCircuit.gridVoltage, [&](float v) {
                         auto& receiver = radio.receiverCircuit;
                         return processResistorLoadedTriodeStage(
                             receiver.tubeBiasVolts + v, 1, receiver.tubeEffectiveSupplyVolts,
                             receiver.tubeQuiescentPlateVolts, receiver.tubeTriodeModel,
                             &receiver.tubeTriodeLut, receiver.tubeLoadResistanceOhms,
                             receiver.tubePlateVoltage);
                       });
    if (i >= 0)
      signal[i] = y;
  }
  return signal;
}

void reportReceiver() {
  constexpr int bin = 683;
  for (auto profile :
       {RadioReceiverProfile::Typical1930s, RadioReceiverProfile::Philco37116}) {
    for (float fs : {48000.0f}) {
      Radio1938 preset;
      preset.applyReceiverProfile(profile);
      const float bias = std::fabs(preset.receiverCircuit.tubeBiasVolts);
      for (float fraction : {.25f, 1.25f}) {
        // Same physical triode at 16 times the host rate (32x nonlinear calls).
        const auto reference = receiverTone(profile, fs, 16, bias * fraction, bin);
        const double referenceAlias = aliasDb(reference, bin, .45 / 16);
        double legacyAlias = 0;
        for (int oversampling : {0, 1, 2, 4, 8}) {
          const auto production =
              receiverTone(profile, fs, 1, bias * fraction, bin, oversampling);
          // Compare the same audible radio band. The legacy resampler rolls
          // off much earlier above it; counting its attenuated high-frequency
          // aliases as an advantage confuses bandwidth with numerical quality.
          const double measuredAlias = aliasDb(production, bin, 8000 / fs);
          if (oversampling == 0)
            legacyAlias = measuredAlias;
          if (oversampling == 2)
            check(measuredAlias <= legacyAlias + .3,
                  "triode passband aliasing regressed against legacy");
          if (oversampling == 8)
            check(measuredAlias < legacyAlias - 10,
                  "higher oversampling did not reduce triode aliases");
          check(referenceAlias < legacyAlias - 20,
                "finer reference is not sufficiently clean");
          std::cout << "receiver," << radioReceiverProfileName(profile) << ',' << fs
                    << ",oversampling=" << oversampling
                    << ",drive_bias_fraction=" << fraction
                    << ",alias_db=" << aliasDb(production, bin)
                    << ",radio_alias_db=" << aliasDb(production, bin, 8000 / fs)
                    << ",reference_alias_db=" << referenceAlias << '\n';
        }
      }
    }
  }
}

void reportProgram(const char* capturePath) {
  std::ofstream capture;
  if (capturePath) {
    capture.open(capturePath, std::ios::binary);
    check(capture.good(), "could not open programme capture");
  }
  constexpr int count = 24000, start = 9600;
  for (auto profile :
       {RadioReceiverProfile::Typical1930s, RadioReceiverProfile::Philco37116}) {
    for (float noise : {0.0f, .012f}) {
      for (int factor : {1, 2, 4, 8}) {
        Radio1938 radio;
        radio.applyReceiverProfile(profile);
        radio.globals.oversampleFactor = static_cast<float>(factor);
        radio.init(1, 48000, 5000, noise);
        radio.warmInputCarrier(.12f, 16384);
        std::vector<float> input(count), output(count), terminal(count);
        for (int i = 0; i < count; ++i)
          input[i] = static_cast<float>(std::sin(2 * kPi * 1000 * i / 48000));
        radio.processAmAudio(input.data(), output.data(), count, .12f, .85f, nullptr,
                             terminal.data());
        double total = 0, mean = 0;
        std::complex<double> fundamental{};
        for (int i = start; i < count; ++i) {
          mean += terminal[i] / double(count - start);
          total += terminal[i] * double(terminal[i]) / (count - start);
          fundamental += static_cast<double>(terminal[i]) *
                         std::polar(2.0 / (count - start), -2 * kPi * 1000 * i / 48000);
        }
        const double signal = .5 * std::norm(fundamental);
        const double residual = std::max(1e-30, total - signal - mean * mean);
        std::cout << "program," << radioReceiverProfileName(profile) << ",noise=" << noise
                  << ",factor=" << factor << ",signal=" << std::sqrt(signal)
                  << ",residual=" << std::sqrt(residual)
                  << ",sinad=" << 10 * std::log10(total / residual) << '\n';
        if (capture)
          capture.write(reinterpret_cast<const char*>(terminal.data()),
                        count * sizeof(float));
      }
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string_view(argv[1]) == "--program-report") {
    reportProgram(argc > 2 ? argv[2] : nullptr);
    return 0;
  }
  reportLegacyPolynomial();
  testStreamingAndRates();
  reportResamplerDelay();
  testPhysicalFilterClock();
  testImageFilter();
  testLimiterImmediatePeak();
  testRfImageRejection();
  reportReceiver();
  std::cout
      << "Oversampling clocks, passbands, images and nonlinear reference checks passed\n";
}
