#include "radio.h"
#include "audiofilter/math/signal_math.h"
#include "audiofilter/radio1938/preview/radio_preview_pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr float kSampleRate = 48000.0f;
constexpr float kBandwidthHz = 5000.0f;
constexpr float kCarrierRmsVolts = 0.12f;
constexpr float kModulationIndex = 0.85f;
constexpr float kTolerance = 1e-5f;
constexpr float kFullScaleSineRms = 0.70710678118654752440f;

constexpr PassId kAllPasses[] = {
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
    PassId::Power,
    PassId::Noise,
    PassId::Speaker,
    PassId::Cabinet,
    PassId::OutputScale,
    PassId::FinalLimiter,
    PassId::OutputClip,
};

void fail(const std::string& message) {
  std::cerr << message << "\n";
  std::exit(1);
}

void disablePipeline(Radio1938& radio) {
  for (PassId pass : kAllPasses) {
    radio.graph.setEnabled(pass, false);
  }
}

Radio1938 makeIngressOnlyRadio() {
  Radio1938 radio;
  radio.init(1, kSampleRate, kBandwidthHz, 0.0f);
  disablePipeline(radio);
  return radio;
}

std::vector<float> makeHotProgram() {
  std::vector<float> program(96, 0.0f);
  for (size_t i = 0; i < program.size(); ++i) {
    const float phase = static_cast<float>(i) * 0.19f;
    program[i] = 0.72f * std::sin(phase);
  }
  program[7] = 1.35f;
  program[21] = -1.28f;
  return program;
}

std::vector<float> processOneBlock(const std::vector<float>& program) {
  Radio1938 radio = makeIngressOnlyRadio();
  std::vector<float> out(program.size(), 0.0f);
  radio.processAmAudio(program.data(), out.data(),
                       static_cast<uint32_t>(program.size()),
                       kCarrierRmsVolts, kModulationIndex);
  return out;
}

std::vector<float> processSplitBlocks(const std::vector<float>& program,
                                      size_t splitFrame) {
  Radio1938 radio = makeIngressOnlyRadio();
  std::vector<float> out(program.size(), 0.0f);
  radio.processAmAudio(program.data(), out.data(),
                       static_cast<uint32_t>(splitFrame),
                       kCarrierRmsVolts, kModulationIndex);
  radio.processAmAudio(program.data() + splitFrame, out.data() + splitFrame,
                       static_cast<uint32_t>(program.size() - splitFrame),
                       kCarrierRmsVolts, kModulationIndex);
  return out;
}

void expectBlockInvariantAmIngress() {
  const std::vector<float> program = makeHotProgram();
  const std::vector<float> oneBlock = processOneBlock(program);
  const std::vector<float> splitBlocks = processSplitBlocks(program, 32);

  float maxDiff = 0.0f;
  for (size_t i = 0; i < oneBlock.size(); ++i) {
    maxDiff = std::max(maxDiff, std::fabs(oneBlock[i] - splitBlocks[i]));
  }
  if (maxDiff > kTolerance) {
    fail("AM ingress output depends on block size; max diff=" +
         std::to_string(maxDiff));
  }
}

void expectHotProgramLevelReachesAmIngress() {
  Radio1938 radio = makeIngressOnlyRadio();
  const float program = 1.35f;
  float out = 0.0f;
  radio.processAmAudio(&program, &out, 1, kCarrierRmsVolts,
                       kModulationIndex);
  const float expected = std::sqrt(2.0f) * kCarrierRmsVolts *
                         (1.0f + kModulationIndex * program);
  if (std::fabs(out - expected) > kTolerance) {
    fail("AM ingress changed hot program level; expected=" +
         std::to_string(expected) + " actual=" + std::to_string(out));
  }
}

void expectTerminalObservationPreservesPlayback() {
  constexpr uint32_t kFrames = 4096;
  std::vector<float> program(kFrames);
  std::vector<RadioAmReceptionSample> reception(kFrames);
  for (uint32_t i = 0; i < kFrames; ++i) {
    program[i] = 0.5f * std::sin(0.13f * i);
    reception[i].desiredCarrierI = 0.92f;
    reception[i].desiredCarrierQ = 0.03f;
  }
  for (auto profile : {RadioReceiverProfile::Typical1930s,
                       RadioReceiverProfile::Philco37116}) {
    Radio1938 plain;
    plain.applyReceiverProfile(profile);
    // AFC publishes tuning changes once per block. Hold tuning fixed to test
    // ingress/observation partitioning independently of that control cadence.
    plain.tuning.magneticTuningEnabled = false;
    plain.init(2, kSampleRate, kBandwidthHz, 0.012f);
    plain.demod.am.waveformMaxSubsteps = 1;
    plain.warmInputCarrier(kCarrierRmsVolts, 512);
    Radio1938 observed = plain;
    Radio1938 split = plain;
    std::vector<float> expected(kFrames * 2), actual(kFrames * 2), divided(kFrames * 2);
    std::vector<float> terminal(kFrames), splitTerminal(kFrames);
    plain.processAmAudio(program.data(), expected.data(), kFrames,
                          kCarrierRmsVolts, kModulationIndex, reception.data());
    observed.processAmAudio(program.data(), actual.data(), kFrames,
                             kCarrierRmsVolts, kModulationIndex, reception.data(),
                             terminal.data());
    for (uint32_t offset = 0; offset < kFrames; offset += 127) {
      split.processAmAudio(program.data() + offset, divided.data() + offset * 2,
                            std::min(127u, kFrames - offset), kCarrierRmsVolts,
                            kModulationIndex, reception.data() + offset,
                            splitTerminal.data() + offset);
    }
    if (expected != actual) fail("Terminal observation changed playback samples.");
    float peak = 0.0f;
    for (uint32_t i = 0; i < kFrames; ++i) {
      peak = std::max(peak, std::fabs(terminal[i]));
      if (!std::isfinite(terminal[i]) ||
          std::fabs(terminal[i] - splitTerminal[i]) > 1e-3f) {
        fail("Terminal observation changed with block partitioning: " +
             std::string(radioReceiverProfileName(profile)) + " frame=" +
             std::to_string(i) + " difference=" +
             std::to_string(terminal[i] - splitTerminal[i]));
      }
      for (uint32_t channel = 0; channel < 2; ++channel) {
        if (std::fabs(actual[i * 2 + channel] - divided[i * 2 + channel]) > 1e-4f) {
          fail("Observed audio changed with block partitioning.");
        }
      }
    }
    if (peak < 0.01f || terminal.back() != observed.speakerStage.physicalDriveVolts) {
      fail("Observation did not capture actual speaker terminal voltage.");
    }
  }
}

void expectReceptionProfileContract() {
  if (kDefaultRadioReceptionProfile !=
      RadioReceptionProfile::Everyday1938) {
    fail("The default radio reception profile is not everyday-1938.");
  }

  RadioAmIngressConfig defaultIngress;
  if (!defaultIngress.reception.enabled ||
      defaultIngress.reception.profile !=
          RadioReceptionProfile::Everyday1938) {
    fail("Default AM ingress does not enable everyday-1938 reception.");
  }

  RadioReceptionProfile parsed = RadioReceptionProfile::StrongLocal;
  if (!parseRadioReceptionProfile("everyday-1938", parsed) ||
      parsed != RadioReceptionProfile::Everyday1938 ||
      radioReceptionProfileName(parsed) != "everyday-1938") {
    fail("everyday-1938 reception profile does not round-trip.");
  }
  if (!parseRadioReceptionProfile("strong-local", parsed) ||
      parsed != RadioReceptionProfile::StrongLocal ||
      radioReceptionProfileName(parsed) != "strong-local") {
    fail("strong-local reception profile does not round-trip.");
  }
  if (!parseRadioReceptionProfile("night-skywave", parsed) ||
      parsed != RadioReceptionProfile::NightSkywave ||
      radioReceptionProfileName(parsed) != "night-skywave") {
    fail("night-skywave reception profile does not round-trip.");
  }
  if (parseRadioReceptionProfile("weak", parsed) ||
      parseRadioReceptionProfile("Everyday1938", parsed)) {
    fail("Reception profile parser accepted an undocumented name.");
  }

  const RadioReceptionConfig everyday = radioReceptionConfigForProfile(
      RadioReceptionProfile::Everyday1938);
  if (!everyday.enabled || everyday.skywaveGain <= 0.0f ||
      everyday.skywaveDopplerMinHz < 0.003f ||
      everyday.skywaveDopplerMaxHz > 0.008f ||
      !everyday.intermittentCarrierEnabled ||
      everyday.intermittentWaitMinSeconds < 60.0f) {
    fail("everyday-1938 is not the conservative slow-fading profile.");
  }

  const RadioReceptionConfig strongLocal = radioReceptionConfigForProfile(
      RadioReceptionProfile::StrongLocal);
  if (strongLocal.enabled) {
    fail("strong-local does not preserve ideal reception.");
  }

  // After dark at the fringe of the groundwave area the two paths are
  // comparable, with the same fade rate and a one-hop path delay.
  const RadioReceptionConfig night = radioReceptionConfigForProfile(
      RadioReceptionProfile::NightSkywave);
  if (!night.enabled || std::fabs(night.skywaveGain / night.groundwaveGain - 1) > .25f ||
      night.skywaveDopplerMinHz != everyday.skywaveDopplerMinHz ||
      night.skywaveDelayMinMs < 0.1f || night.skywaveDelayMaxMs > 1.0f ||
      everyday.skywaveDelayMinMs != night.skywaveDelayMinMs) {
    fail("night-skywave is not a comparable-path, one-hop profile.");
  }
}

// Ingress-level check of selective fading: with the skywave carrier in
// antiphase and equal strength the carrier vanishes, while sidebands whose
// path phase 2*pi*f*delay is an odd multiple of pi add instead.
void expectDelayedSkywaveFadesSelectively() {
  constexpr float kDelaySamples = 12.0f;  // 0.25 ms
  constexpr float kToneHz = 2000.0f;      // 2*pi*f*delay = pi
  constexpr uint32_t kFrames = 48000u;
  Radio1938 radio = makeIngressOnlyRadio();
  std::vector<float> program(kFrames), output(kFrames);
  std::vector<RadioAmReceptionSample> reception(kFrames);
  for (uint32_t i = 0; i < kFrames; ++i) {
    program[i] = std::sin(kRadioTwoPi * kToneHz * i / kSampleRate);
    reception[i].desiredCarrierI = 0.5f;
    reception[i].desiredCarrierQ = 0.0f;
    reception[i].skywaveI = -0.5f;
    reception[i].skywaveQ = 0.0f;
    reception[i].skywaveDelaySamples = kDelaySamples;
  }
  const float m = 0.5f;
  radio.processAmAudio(program.data(), output.data(), kFrames, kCarrierRmsVolts, m,
                       reception.data());
  const float carrierHz = radio.resolvedInputCarrierHz();
  const auto amplitude = [&](float hz) {
    double re = 0, im = 0;
    for (uint32_t i = kFrames / 2; i < kFrames; ++i) {
      re += output[i] * std::cos(kRadioTwoPi * hz * i / kSampleRate);
      im += output[i] * std::sin(kRadioTwoPi * hz * i / kSampleRate);
    }
    return 2 * std::hypot(re, im) / (kFrames / 2);
  };
  const double peak = std::sqrt(2.0) * kCarrierRmsVolts;
  const double carrier = amplitude(carrierHz);
  const double upper = amplitude(carrierHz + kToneHz);
  const double lower = amplitude(carrierHz - kToneHz);
  // Each sideband of the direct wave is m/2 * peak * 0.5; the skywave adds an
  // equal one in phase.
  const double expectedSideband = 0.5 * m * peak;
  if (carrier > 1e-3 * peak || std::fabs(upper / expectedSideband - 1) > .02 ||
      std::fabs(lower / expectedSideband - 1) > .02) {
    fail("Delayed skywave does not fade selectively; carrier=" + std::to_string(carrier) +
         " upper=" + std::to_string(upper) + " lower=" + std::to_string(lower));
  }
}

void expectReceptionSidecarShapesRf() {
  Radio1938 radio = makeIngressOnlyRadio();
  radio.iqInput.carrierOscCos = 0.0f;
  radio.iqInput.carrierOscSin = 1.0f;
  const float program = 0.0f;
  float output = 0.0f;
  RadioAmReceptionSample reception;
  reception.desiredCarrierI = 0.0f;
  reception.desiredCarrierQ = 0.5f;
  reception.additiveRf = 0.007f;
  radio.processAmAudio(&program, &output, 1u, kCarrierRmsVolts,
                       kModulationIndex, &reception);

  const float carrierPeak = std::sqrt(2.0f) * kCarrierRmsVolts;
  const float expected = -0.5f * carrierPeak + reception.additiveRf;
  if (std::fabs(output - expected) > kTolerance) {
    fail("AM reception sidecar did not rotate and add RF correctly; expected=" +
         std::to_string(expected) + " actual=" + std::to_string(output));
  }
}

void expectStrongLocalReceptionIsTransparent() {
  Radio1938 radio;
  radio.init(1, kSampleRate, kBandwidthHz, 0.0f);
  RadioAmIngressConfig ingress;
  ingress.reception = radioReceptionConfigForProfile(
      RadioReceptionProfile::StrongLocal);
  // Strong local reception has no propagation effects; its external noise
  // is checked separately.
  ingress.reception.externalNoiseEnabled = false;
  RadioPreviewConfig config;
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, config, kSampleRate);

  RadioPreviewSampleContext ctx{};
  RadioAmReceptionSample reception;
  ctx.reception = &reception;
  for (uint32_t i = 0; i < 4096u; ++i) {
    const float input = std::sin(static_cast<float>(i) * 0.071f);
    const float output = RadioPreviewReceptionNode::run(preview, input, ctx);
    if (output != input || reception.desiredCarrierI != 1.0f ||
        reception.desiredCarrierQ != 0.0f || reception.additiveRf != 0.0f) {
      fail("strong-local reception is not sample-transparent.");
    }
  }

  const std::vector<float> program = makeHotProgram();
  std::vector<RadioAmReceptionSample> identity(program.size());
  Radio1938 ideal = makeIngressOnlyRadio();
  Radio1938 sidecar = makeIngressOnlyRadio();
  std::vector<float> idealOutput(program.size(), 0.0f);
  std::vector<float> sidecarOutput(program.size(), 0.0f);
  ideal.processAmAudio(program.data(), idealOutput.data(),
                       static_cast<uint32_t>(program.size()),
                       kCarrierRmsVolts, kModulationIndex);
  sidecar.processAmAudio(program.data(), sidecarOutput.data(),
                         static_cast<uint32_t>(program.size()),
                         kCarrierRmsVolts, kModulationIndex,
                         identity.data());
  for (size_t i = 0; i < program.size(); ++i) {
    if (idealOutput[i] != sidecarOutput[i]) {
      fail("Identity reception sidecar changed ideal AM ingress.");
    }
  }
}

// FCC 1939 primary-service fields against ITU-R P.372 median man-made noise
// at 1 MHz: residential 72.5 dB and rural 67.2 dB above kT0b, eq. (7).
void expectExternalNoiseFollowsFieldAndNoiseFigure() {
  struct Case {
    RadioReceptionProfile profile;
    double expectedDbHz;
  };
  // 20 log10(uV/m at unit path gain) - (Fa - 95.5).
  const Case cases[] = {
      {RadioReceptionProfile::Everyday1938,
       20.0 * std::log10(2000.0 / 0.92) - (72.5 - 95.5)},
      {RadioReceptionProfile::StrongLocal,
       20.0 * std::log10(10000.0) - (72.5 - 95.5)},
      {RadioReceptionProfile::NightSkywave,
       20.0 * std::log10(500.0 / 0.60) - (67.2 - 95.5)},
  };
  for (const auto& item : cases) {
    const RadioReceptionConfig reception = radioReceptionConfigForProfile(item.profile);
    const double cn0 = radioReceptionCarrierToManMadeNoiseDbHz(reception, 1e6);
    if (!reception.externalNoiseEnabled || std::fabs(cn0 - item.expectedDbHz) > 1e-3)
      fail("external noise C/N0 does not follow the FCC field and P.372 noise: " +
           std::to_string(cn0) + " vs " + std::to_string(item.expectedDbHz));
    // The generated RF noise must have that density at every host rate:
    // one-sided N0 = 2 var / fs against carrier power Vrms^2.
    for (float rate : {44100.0f, 48000.0f, 96000.0f}) {
      Radio1938 radio;
      radio.init(1, rate, kBandwidthHz, 0.0f);
      RadioAmIngressConfig ingress;
      ingress.reception = reception;
      ingress.reception.enabled = false;
      // Atmospheric noise is checked separately below.
      ingress.reception.atmosphericNoiseEnabled = false;
      RadioPreviewPipeline preview;
      preview.initialize(radio, ingress, RadioPreviewConfig{}, rate);
      RadioPreviewSampleContext ctx{};
      RadioAmReceptionSample sample;
      ctx.reception = &sample;
      double sumSq = 0.0;
      constexpr uint32_t kSamples = 400000u;
      for (uint32_t i = 0; i < kSamples; ++i) {
        RadioPreviewReceptionNode::run(preview, 0.0f, ctx);
        sumSq += static_cast<double>(sample.additiveRf) * sample.additiveRf;
      }
      const double n0 = 2.0 * (sumSq / kSamples) / rate;
      const double measured =
          10.0 * std::log10(double(radioReceptionAntennaCarrierRmsVolts(ingress.reception)) *
                            radioReceptionAntennaCarrierRmsVolts(ingress.reception) / n0);
      // Propagation disabled: the carrier arrives at unit path gain.
      const double generated =
          radioReceptionCarrierToManMadeNoiseDbHz(ingress.reception, 1e6);
      if (std::fabs(measured - generated) > 0.05)
        fail("generated RF noise density is wrong: " + std::to_string(measured) +
             " dB-Hz, expected " + std::to_string(generated));
    }
  }
}

// ITU-R P.372 evening atmospheric noise for night-skywave: Fam 80 dB at 1 MHz
// (spring, 2000-2400 LT, north-eastern United States, Fig. 26a). Its power
// is shared between a Gaussian part and log-normal sferics; the sferic mean
// power is dominated by rare large strokes, so the generator is checked
// through its rate and amplitude statistics and the closed-form power.
void expectAtmosphericNoiseFollowsP372() {
  const RadioReceptionConfig night =
      radioReceptionConfigForProfile(RadioReceptionProfile::NightSkywave);
  const double expected = 20.0 * std::log10(500.0 / 0.60) - (80.0 - 95.5);
  if (!night.atmosphericNoiseEnabled ||
      std::fabs(radioReceptionCarrierToAtmosphericNoiseDbHz(night, 1e6) - expected) > 1e-3)
    fail("night atmospheric noise does not follow P.372");
  if (radioReceptionConfigForProfile(RadioReceptionProfile::Everyday1938)
          .atmosphericNoiseEnabled ||
      radioReceptionConfigForProfile(RadioReceptionProfile::StrongLocal)
          .atmosphericNoiseEnabled)
    fail("daytime profiles must not carry skywave atmospheric noise");
  for (float rate : {44100.0f, 96000.0f}) {
    Radio1938 radio;
    radio.init(1, rate, kBandwidthHz, 0.0f);
    RadioAmIngressConfig ingress;
    ingress.reception = night;
    ingress.reception.enabled = false;
    ingress.reception.manMadeNoiseC = -200.0f;  // isolate the atmospheric part
    RadioPreviewPipeline preview;
    preview.initialize(radio, ingress, RadioPreviewConfig{}, rate);
    const auto& runtime = preview.receptionRuntime;
    const double carrier = radioReceptionAntennaCarrierRmsVolts(ingress.reception);
    const double cn0 = radioReceptionCarrierToAtmosphericNoiseDbHz(ingress.reception, 1e6);
    const double variance = carrier * carrier * 0.5 * rate * std::pow(10.0, -cn0 / 10.0);
    const double g = night.atmosphericGaussianPowerFraction;
    const double sigma = night.sfericLogAmplitudeSigma;
    const double sfericVariance = runtime.sfericProbability * double(runtime.sfericMedianVolts) *
                                  runtime.sfericMedianVolts * std::exp(2 * sigma * sigma);
    if (std::fabs(runtime.externalNoiseSigma * double(runtime.externalNoiseSigma) /
                      (g * variance) - 1) > 1e-3 ||
        std::fabs(sfericVariance / ((1 - g) * variance) - 1) > 1e-3)
      fail("atmospheric noise power is not shared as configured");
    RadioPreviewSampleContext ctx{};
    RadioAmReceptionSample sample;
    ctx.reception = &sample;
    constexpr uint32_t kSamples = 2000000u;
    uint32_t impulses = 0;
    double logSum = 0.0;
    const double gaussianLimit = 8.0 * runtime.externalNoiseSigma;
    for (uint32_t i = 0; i < kSamples; ++i) {
      RadioPreviewReceptionNode::run(preview, 0.0f, ctx);
      // Sferics far above the Gaussian part are counted; the smallest
      // ones are hidden in it, so the fit uses the upper part only.
      const double a = std::fabs(sample.additiveRf);
      if (a > gaussianLimit) {
        ++impulses;
        const double l = std::log(a / runtime.sfericMedianVolts);
        logSum += l;
      }
    }
    // Fraction of log-normal impulses above the threshold, from the normal
    // tail; with rate r the count follows.
    const double z = std::log(gaussianLimit / runtime.sfericMedianVolts) / sigma;
    const double above = 0.5 * std::erfc(z / std::sqrt(2.0));
    const double expectedCount = runtime.sfericProbability * kSamples * above;
    // Mean log amplitude of a normal tail above z: sigma phi(z) / Q(z).
    const double tailMean =
        sigma * std::exp(-0.5 * z * z) / std::sqrt(2 * 3.14159265358979323846) / above;
    if (std::fabs(impulses / expectedCount - 1) > 0.05 ||
        std::fabs(logSum / impulses - tailMean) > 0.05 * sigma)
      fail("sferic rate or amplitude spread is wrong: " + std::to_string(impulses) +
           " vs " + std::to_string(expectedCount));
  }
}

// Lightning crashes at night overdrive the receiver; it must recover. A
// crash once drove the Philco's output-transformer primary to thousands of
// volts because its triode table clamped the plate voltage, turning the
// tubes into current sources that then drained the B+ below zero.
void expectReceiversRecoverFromLightningCrashes() {
  constexpr float kRate = 48000.0f;
  for (auto profile : {RadioReceiverProfile::Typical1930s, RadioReceiverProfile::Philco37116}) {
    Radio1938 radio;
    radio.applyReceiverProfile(profile);
    radio.init(1, kRate, kBandwidthHz, 0.012f);
    RadioAmIngressConfig ingress;
    ingress.reception = radioReceptionConfigForProfile(RadioReceptionProfile::NightSkywave);
    RadioPreviewPipeline preview;
    preview.initialize(radio, ingress, RadioPreviewConfig{}, kRate);
    const double idle = radio.powerSupply.mainReferenceVolts;
    std::vector<float> block(2400);
    double lowest = idle;
    for (int n = 0; n < 160; ++n) {  // 8 s
      std::fill(block.begin(), block.end(), 0.0f);
      preview.runBlock(radio, block.data(), static_cast<uint32_t>(block.size()), 1);
      lowest = std::min(lowest, radio.powerSupply.circuit.volts(RectifierSupply::Main));
      for (float v : block)
        if (!std::isfinite(v)) fail("non-finite output after a lightning crash");
    }
    if (lowest < 0.6 * idle)
      fail(std::string(radioReceiverProfileName(profile)) +
           " B+ collapsed after a lightning crash: " + std::to_string(lowest) + " V");
    if (std::fabs(radio.powerSupply.circuit.volts(RectifierSupply::Main) / idle - 1) > 0.05)
      fail("B+ did not recover after lightning crashes");
  }
}

void expectEverydayReceptionProducesSlowCoherentFading() {
  Radio1938 radio;
  radio.init(1, kSampleRate, kBandwidthHz, 0.0f);
  RadioAmIngressConfig ingress;
  auto& receptionConfig = ingress.reception;
  receptionConfig.skywaveAmplitudeVariationRms = 0.0f;
  receptionConfig.skywaveDopplerMinHz = 0.5f;
  receptionConfig.skywaveDopplerMaxHz = 0.5f;
  receptionConfig.skywaveDopplerWanderRmsHz = 0.0f;
  receptionConfig.intermittentCarrierEnabled = false;
  receptionConfig.externalNoiseEnabled = false;
  RadioPreviewConfig config;
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, config, kSampleRate);

  RadioPreviewSampleContext ctx{};
  RadioAmReceptionSample reception;
  ctx.reception = &reception;
  float minimumMagnitude = 100.0f;
  float maximumMagnitude = 0.0f;
  constexpr uint32_t kFrames = 2u * 48000u;
  for (uint32_t i = 0; i < kFrames; ++i) {
    RadioPreviewReceptionNode::run(preview, 0.0f, ctx);
    // The unmodulated carrier sees both paths; delay only affects sidebands.
    const float magnitude = std::hypot(reception.desiredCarrierI + reception.skywaveI,
                                       reception.desiredCarrierQ + reception.skywaveQ);
    minimumMagnitude = std::min(minimumMagnitude, magnitude);
    maximumMagnitude = std::max(maximumMagnitude, magnitude);
    if (reception.additiveRf != 0.0f) {
      fail("Disabled intermittent carrier still injected RF.");
    }
  }

  const float expectedMinimum = receptionConfig.groundwaveGain -
                                receptionConfig.skywaveGain;
  const float expectedMaximum = receptionConfig.groundwaveGain +
                                receptionConfig.skywaveGain;
  if (std::fabs(minimumMagnitude - expectedMinimum) > 0.003f ||
      std::fabs(maximumMagnitude - expectedMaximum) > 0.003f) {
    fail("Groundwave and skywave do not combine as coherent RF paths; min=" +
         std::to_string(minimumMagnitude) + " max=" +
         std::to_string(maximumMagnitude));
  }
}

void expectIntermittentCarrierIsRareRfInsteadOfPermanentWhistle() {
  Radio1938 radio;
  radio.init(1, kSampleRate, kBandwidthHz, 0.0f);
  RadioAmIngressConfig ingress;
  // Only the interfering carrier is under test, not the broadband noise.
  ingress.reception.externalNoiseEnabled = false;
  RadioPreviewConfig config;
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, config, kSampleRate);

  RadioPreviewSampleContext ctx{};
  RadioAmReceptionSample reception;
  ctx.reception = &reception;
  for (uint32_t i = 0; i < 48000u; ++i) {
    RadioPreviewReceptionNode::run(preview, 0.0f, ctx);
    if (reception.additiveRf != 0.0f) {
      fail("Default reception injects a permanent RF carrier during its "
           "minimum quiet interval.");
    }
  }

  auto& receptionConfig = ingress.reception;
  receptionConfig.groundwaveGain = 1.0f;
  receptionConfig.skywaveGain = 0.0f;
  receptionConfig.intermittentWaitMinSeconds = 0.002f;
  receptionConfig.intermittentWaitMaxSeconds = 0.002f;
  receptionConfig.intermittentDurationMinSeconds = 0.010f;
  receptionConfig.intermittentDurationMaxSeconds = 0.010f;
  receptionConfig.intermittentLevelMinDb = -30.0f;
  receptionConfig.intermittentLevelMaxDb = -30.0f;
  receptionConfig.intermittentOffsetMinHz = 500.0f;
  receptionConfig.intermittentOffsetMaxHz = 500.0f;
  receptionConfig.intermittentAttackMs = 0.1f;
  receptionConfig.intermittentReleaseMs = 0.1f;
  preview.initialize(radio, ingress, config, kSampleRate);

  uint32_t zeroFrames = 0u;
  uint32_t carrierFrames = 0u;
  for (uint32_t i = 0; i < 4800u; ++i) {
    RadioPreviewReceptionNode::run(preview, 0.0f, ctx);
    if (reception.additiveRf == 0.0f) {
      ++zeroFrames;
    } else {
      ++carrierFrames;
    }
  }
  if (zeroFrames == 0u || carrierFrames == 0u) {
    fail("Intermittent RF carrier did not alternate between quiet and active "
         "reception.");
  }
}

void expectPreviewCapsDetectorWork() {
  Radio1938 radio;
  radio.init(1, kSampleRate, kBandwidthHz, 0.0f);
  RadioAmIngressConfig ingress;
  RadioPreviewConfig config;
  config.detectorMaxSubsteps = 1;
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, config, kSampleRate);
  if (radio.demod.am.waveformMaxSubsteps != 1) {
    fail("Radio preview did not apply realtime detector substep budget.");
  }
}

float measurePreviewProgramFilterRms(float frequencyHz, float receiverBw = kBandwidthHz) {
  Radio1938 radio;
  radio.init(1, kSampleRate, receiverBw, 0.0f);
  RadioAmIngressConfig ingress;
  RadioPreviewConfig config;
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, config, kSampleRate);

  RadioPreviewSampleContext ctx{};
  double sumSq = 0.0;
  constexpr uint32_t kFrames = 48000u;
  constexpr uint32_t kAnalysisStart = kFrames / 2u;
  for (uint32_t i = 0; i < kFrames; ++i) {
    const double phase = 6.28318530717958647692 * frequencyHz * i / kSampleRate;
    const float out =
        RadioPreviewProgramFilterNode::run(preview, static_cast<float>(std::sin(phase)), ctx);
    if (i >= kAnalysisStart) {
      sumSq += static_cast<double>(out) * static_cast<double>(out);
    }
  }
  return static_cast<float>(
      std::sqrt(sumSq / static_cast<double>(kFrames - kAnalysisStart)));
}

void expectPreviewUsesRequestedAudioBandwidthOnce() {
  const float passbandRms = measurePreviewProgramFilterRms(500.0f);
  const float edgeRms = measurePreviewProgramFilterRms(std::min(10000.0f, .20f * kSampleRate));
  const float edgeRatio = edgeRms / std::max(passbandRms, 1e-9f);
  if (edgeRatio < 0.68f || edgeRatio > 0.74f) {
    fail("Transmitter audio-bandwidth edge is not a single -3 dB low-pass; ratio=" +
         std::to_string(edgeRatio));
  }
  if (measurePreviewProgramFilterRms(20000) > passbandRms * 1e-5f)
    fail("Transmitter lets ultrasonic programme fold into the AM channel");
  if (std::fabs(measurePreviewProgramFilterRms(6000, 3000) -
                measurePreviewProgramFilterRms(6000, 6500)) > 1e-6f)
    fail("Transmitter bandwidth still follows receiver selectivity");
}

// The 110A and the transmitter alone: no operator riding, no network line.
RadioAmIngressConfig makeStationOnlyConfig() {
  RadioAmIngressConfig ingress;
  ingress.broadcastSource.levelRidingEnabled = false;
  ingress.broadcastSource.programLineHighHz = 0.0f;
  return ingress;
}

RadioAmIngressConfig makeIsolatedBroadcastSourceConfig() {
  RadioAmIngressConfig ingress = makeStationOnlyConfig();
  ingress.broadcastSource.noiseEnabled = false;
  ingress.broadcastSource.nonlinearityEnabled = false;
  return ingress;
}

void expectBroadcastSourceBypassIsTransparent() {
  Radio1938 radio;
  radio.init(1, kSampleRate, kBandwidthHz, 0.0f);
  RadioAmIngressConfig ingress;
  ingress.broadcastSource.enabled = false;
  RadioPreviewConfig config;
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, config, kSampleRate);

  RadioPreviewSampleContext ctx{};
  for (uint32_t i = 0; i < 1024u; ++i) {
    const float input = 1.4f * std::sin(static_cast<float>(i) * 0.173f);
    const float output =
        RadioPreviewBroadcastSourceNode::run(preview, input, ctx);
    if (output != input) {
      fail("Disabled broadcast source is not sample-transparent.");
    }
  }
}

void expectBroadcastCompressionTiming() {
  Radio1938 radio;
  radio.init(1, kSampleRate, kBandwidthHz, 0.0f);
  RadioAmIngressConfig ingress = makeIsolatedBroadcastSourceConfig();
  RadioPreviewConfig config;
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, config, kSampleRate);

  RadioPreviewSampleContext ctx{};
  const auto& source = ingress.broadcastSource;
  const float compressionThreshold =
      source.compressionThresholdModulation / ingress.modulationIndex;
  const uint32_t attackFrames = static_cast<uint32_t>(
      kSampleRate * source.compressionAttackMs * 0.001f);
  for (uint32_t i = 0; i < attackFrames; ++i) {
    RadioPreviewBroadcastSourceNode::run(preview, 1.0f, ctx);
  }
  const float targetGain = std::pow(
      1.0f / compressionThreshold,
      1.0f / source.compressionRatio - 1.0f);
  const float expectedAttackGain =
      targetGain + (1.0f - targetGain) * std::exp(-1.0f);
  if (std::fabs(preview.broadcastGain - expectedAttackGain) > 0.002f) {
    fail("Broadcast compressor does not have the configured 20 ms attack; "
         "gain=" +
         std::to_string(preview.broadcastGain));
  }

  for (uint32_t i = 0; i < attackFrames * 9u; ++i) {
    RadioPreviewBroadcastSourceNode::run(preview, 1.0f, ctx);
  }
  const float envelopeBeforeRelease = preview.broadcastEnvelope;
  const uint32_t releaseFrames = static_cast<uint32_t>(
      kSampleRate * source.compressionReleaseMs * 0.001f);
  for (uint32_t i = 0; i < releaseFrames; ++i) {
    RadioPreviewBroadcastSourceNode::run(preview, 0.0f, ctx);
  }
  const float expectedRelease = envelopeBeforeRelease * std::exp(-1.0f);
  if (std::fabs(preview.broadcastEnvelope - expectedRelease) > 0.002f) {
    fail("Broadcast compressor does not have the configured 250 ms release; "
         "envelope=" +
         std::to_string(preview.broadcastEnvelope));
  }
}

void expectBroadcastCompressionSlope() {
  Radio1938 radio;
  radio.init(1, kSampleRate, kBandwidthHz, 0.0f);
  RadioAmIngressConfig ingress = makeIsolatedBroadcastSourceConfig();
  RadioPreviewConfig config;
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, config, kSampleRate);

  const auto& source = ingress.broadcastSource;
  const float compressionThreshold =
      source.compressionThresholdModulation / ingress.modulationIndex;
  const float inputPeak =
      compressionThreshold * db2lin(5.0f);
  const float expectedOutputPeak =
      compressionThreshold * db2lin(5.0f / source.compressionRatio);
  RadioPreviewSampleContext ctx{};
  constexpr uint32_t kFrames = 3u * 48000u;
  constexpr uint32_t kAnalysisStart = 2u * 48000u;
  double sumSq = 0.0;
  for (uint32_t i = 0; i < kFrames; ++i) {
    const float phase = kRadioTwoPi * 1000.0f *
                        (static_cast<float>(i) / kSampleRate);
    const float output = RadioPreviewBroadcastSourceNode::run(
        preview, inputPeak * std::sin(phase), ctx);
    if (i >= kAnalysisStart) {
      sumSq += static_cast<double>(output) * static_cast<double>(output);
    }
  }
  const float outputPeak = static_cast<float>(
      std::sqrt(2.0 * sumSq /
                static_cast<double>(kFrames - kAnalysisStart)));
  if (std::fabs(outputPeak - expectedOutputPeak) > 0.025f) {
    fail("Broadcast compression is not 2.5:1 above threshold; expected peak=" +
         std::to_string(expectedOutputPeak) + " actual=" +
         std::to_string(outputPeak));
  }
}

void expectBroadcastNonlinearityNearOnePercentThd() {
  Radio1938 radio;
  radio.init(1, kSampleRate, kBandwidthHz, 0.0f);
  RadioAmIngressConfig ingress = makeStationOnlyConfig();
  ingress.broadcastSource.compressionEnabled = false;
  ingress.broadcastSource.noiseEnabled = false;
  RadioPreviewConfig config;
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, config, kSampleRate);

  RadioPreviewSampleContext ctx{};
  constexpr uint32_t kFrames = 48000u;
  double fundamentalSin = 0.0;
  double fundamentalCos = 0.0;
  double thirdSin = 0.0;
  double thirdCos = 0.0;
  for (uint32_t i = 0; i < kFrames; ++i) {
    const double phase = static_cast<double>(kRadioTwoPi) * 1000.0 *
                         (static_cast<double>(i) / kSampleRate);
    const float output = RadioPreviewBroadcastSourceNode::run(
        preview, static_cast<float>(std::sin(phase)), ctx);
    fundamentalSin += output * std::sin(phase);
    fundamentalCos += output * std::cos(phase);
    thirdSin += output * std::sin(3.0 * phase);
    thirdCos += output * std::cos(3.0 * phase);
  }
  const double fundamental =
      std::hypot(fundamentalSin, fundamentalCos);
  const double third = std::hypot(thirdSin, thirdCos);
  const float thd = static_cast<float>(third / fundamental);
  if (thd < 0.009f || thd > 0.012f) {
    fail("Broadcast nonlinearity is outside its period-transmitter target; "
         "THD=" +
         std::to_string(thd));
  }
}

// Steady sine through the broadcast source alone (no 110A, transmitter
// term or noise); returns the output amplitude over the last half second.
float broadcastSourceSineAmplitude(const RadioAmIngressConfig& ingress, float hz,
                                   float amplitude, float seconds) {
  Radio1938 radio;
  radio.init(1, kSampleRate, kBandwidthHz, 0.0f);
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, RadioPreviewConfig{}, kSampleRate);
  RadioPreviewSampleContext ctx{};
  const auto frames = static_cast<uint32_t>(seconds * kSampleRate);
  const auto tail = static_cast<uint32_t>(0.5f * kSampleRate);
  double re = 0, im = 0;
  for (uint32_t i = 0; i < frames; ++i) {
    const double phase = 2.0 * 3.14159265358979323846 * hz * i / kSampleRate;
    const float output = RadioPreviewBroadcastSourceNode::run(
        preview, amplitude * static_cast<float>(std::sin(phase)), ctx);
    if (i >= frames - tail) {
      re += output * std::cos(phase);
      im += output * std::sin(phase);
    }
  }
  return static_cast<float>(2.0 * std::hypot(re, im) / tail);
}

RadioAmIngressConfig makeSourceStageConfig(bool riding, bool line) {
  RadioAmIngressConfig ingress = makeIsolatedBroadcastSourceConfig();
  ingress.broadcastSource.compressionEnabled = false;
  ingress.broadcastSource.levelRidingEnabled = riding;
  if (line) ingress.broadcastSource.programLineHighHz = 8000.0f;
  return ingress;
}

void expectNetworkProgramLine() {
  const auto ingress = makeSourceStageConfig(false, true);
  const auto db = [&](float hz) {
    return 20.0f * std::log10(broadcastSourceSineAmplitude(ingress, hz, 0.5f, 1.5f) / 0.5f);
  };
  const float mid = db(1000.0f), edge = db(8000.0f), above = db(10000.0f),
              low = db(50.0f);
  std::cout << "network line dB: 1 kHz " << mid << ", 50 Hz " << low << ", 8 kHz "
            << edge << ", 10 kHz " << above << "\n";
  if (std::fabs(mid) > 0.1f || std::fabs(edge + 3.0f) > 0.5f || above > -15.0f ||
      std::fabs(low + 3.0f) > 0.5f)
    fail("Network programme line is not the 50-8000 cycle wide-band circuit");
}

void expectOperatorRidesToTheLimiter() {
  const auto ingress = makeSourceStageConfig(true, false);
  const float threshold = ingress.broadcastSource.compressionThresholdModulation /
                          ingress.modulationIndex;
  // A quiet and a loud programme both reach the limiter threshold, within the
  // operator's 20 dB of boost and 12 dB of cut.
  for (float amplitude : {0.2f, 0.94f, 2.0f}) {
    const float output = broadcastSourceSineAmplitude(ingress, 1000.0f, amplitude, 6.0f);
    if (std::fabs(output - threshold) > 0.02f * threshold)
      fail("Operator did not ride a " + std::to_string(amplitude) +
           " programme to the limiter; output=" + std::to_string(output));
  }
  // Below the gate (a pause) the gain holds.
  Radio1938 radio;
  radio.init(1, kSampleRate, kBandwidthHz, 0.0f);
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, RadioPreviewConfig{}, kSampleRate);
  RadioPreviewSampleContext ctx{};
  for (uint32_t i = 0; i < 3u * 48000u; ++i)
    RadioPreviewBroadcastSourceNode::run(
        preview, 0.3f * std::sin(0.13f * static_cast<float>(i)), ctx);
  const float held = preview.levelRidingGain;
  for (uint32_t i = 0; i < 5u * 48000u; ++i)
    RadioPreviewBroadcastSourceNode::run(preview, 0.0f, ctx);
  // Within 0.1 dB: the 50 ms meter needs about 0.1 s to recognise the pause.
  if (std::fabs(preview.levelRidingGain / held - 1.0f) > 0.012f)
    fail("Operator raised the gain during a pause; " + std::to_string(held) + " -> " +
         std::to_string(preview.levelRidingGain));
}

void expectBroadcastNoiseIsBroadbandWithoutWhistle() {
  Radio1938 radio;
  radio.init(1, kSampleRate, kBandwidthHz, 0.0f);
  RadioAmIngressConfig ingress;
  ingress.broadcastSource.compressionEnabled = false;
  ingress.broadcastSource.nonlinearityEnabled = false;
  RadioPreviewConfig config;
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, config, kSampleRate);

  constexpr uint32_t kWarmupFrames = 48000u;
  constexpr uint32_t kAnalysisFrames = 3u * 48000u;
  constexpr size_t kToneCount = 24u;
  std::vector<double> oscillatorSin(kToneCount, 0.0);
  std::vector<double> oscillatorCos(kToneCount, 1.0);
  std::vector<double> stepSin(kToneCount, 0.0);
  std::vector<double> stepCos(kToneCount, 0.0);
  std::vector<double> projectionSin(kToneCount, 0.0);
  std::vector<double> projectionCos(kToneCount, 0.0);
  for (size_t tone = 0; tone < kToneCount; ++tone) {
    const double frequencyHz = 250.0 + 200.0 * static_cast<double>(tone);
    const double step = static_cast<double>(kRadioTwoPi) * frequencyHz /
                        static_cast<double>(kSampleRate);
    stepSin[tone] = std::sin(step);
    stepCos[tone] = std::cos(step);
  }

  RadioPreviewSampleContext ctx{};
  RadioAmReceptionSample reception;
  ctx.reception = &reception;
  double sum = 0.0;
  double sumSq = 0.0;
  for (uint32_t i = 0; i < kWarmupFrames + kAnalysisFrames; ++i) {
    const float output = runRadioPreviewPipelineSample(
        preview, 0.0f, ctx, RadioPreviewPassId::BroadcastSource);
    if (i >= kWarmupFrames) {
      sum += output;
      sumSq += static_cast<double>(output) * static_cast<double>(output);
      for (size_t tone = 0; tone < kToneCount; ++tone) {
        projectionSin[tone] += output * oscillatorSin[tone];
        projectionCos[tone] += output * oscillatorCos[tone];
      }
    }
    for (size_t tone = 0; tone < kToneCount; ++tone) {
      const double nextCos = oscillatorCos[tone] * stepCos[tone] -
                             oscillatorSin[tone] * stepSin[tone];
      const double nextSin = oscillatorSin[tone] * stepCos[tone] +
                             oscillatorCos[tone] * stepSin[tone];
      oscillatorCos[tone] = nextCos;
      oscillatorSin[tone] = nextSin;
    }
  }

  const double noiseRms =
      std::sqrt(sumSq / static_cast<double>(kAnalysisFrames));
  const float noiseBelowFullModulationDb = static_cast<float>(
      20.0 * std::log10(kFullScaleSineRms / std::max(noiseRms, 1e-12)));
  if (noiseBelowFullModulationDb < 63.0f ||
      noiseBelowFullModulationDb > 65.0f) {
    fail("Broadcast noise misses its -64 dB full-modulation reference; dB=" +
         std::to_string(noiseBelowFullModulationDb));
  }

  const double mean = sum / static_cast<double>(kAnalysisFrames);
  if (std::fabs(mean) > noiseRms * 0.02) {
    fail("Broadcast noise has a DC bias.");
  }
  double strongestTone = 0.0;
  for (size_t tone = 0; tone < kToneCount; ++tone) {
    const double amplitude =
        2.0 * std::hypot(projectionSin[tone], projectionCos[tone]) /
        static_cast<double>(kAnalysisFrames);
    strongestTone = std::max(strongestTone, amplitude);
  }
  if (strongestTone > noiseRms * 0.03) {
    fail("Broadcast source contains a coherent whistle; tone/noise ratio=" +
         std::to_string(strongestTone / noiseRms));
  }
}

struct PreviewProgramResult {
  std::vector<float> output;
  std::vector<RadioAmReceptionSample> reception;
};

PreviewProgramResult processPreviewProgramBlocks(
    const std::vector<float>& program,
    size_t splitFrame) {
  Radio1938 radio;
  radio.init(1, kSampleRate, kBandwidthHz, 0.0f);
  RadioAmIngressConfig ingress;
  RadioPreviewConfig config;
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, config, kSampleRate);
  preview.warmedUp = true;

  PreviewProgramResult result;
  result.output.resize(program.size(), 0.0f);
  result.reception.resize(program.size());
  auto processRange = [&](size_t first, size_t count) {
    RadioPreviewBlockControl block{};
    beginRadioPreviewPipelineBlock(preview, static_cast<uint32_t>(count),
                                   block);
    RadioPreviewSampleContext ctx{};
    ctx.block = &block;
    for (size_t i = first; i < first + count; ++i) {
      ctx.reception = &result.reception[i];
      result.output[i] = runRadioPreviewPipelineSample(
          preview, program[i], ctx, RadioPreviewPassId::BroadcastSource);
    }
  };

  if (splitFrame == 0u) {
    processRange(0u, program.size());
  } else {
    processRange(0u, splitFrame);
    processRange(splitFrame, program.size() - splitFrame);
  }
  return result;
}

void expectPreviewSourceAndReceptionAreBlockInvariant() {
  std::vector<float> program(4096u, 0.0f);
  for (size_t i = 0; i < program.size(); ++i) {
    program[i] = 0.9f * std::sin(static_cast<float>(i) * 0.071f);
  }
  program[701] = 1.4f;
  program[2703] = -1.3f;

  const PreviewProgramResult oneBlock =
      processPreviewProgramBlocks(program, 0u);
  const PreviewProgramResult splitBlocks =
      processPreviewProgramBlocks(program, 1237u);
  float maxDiff = 0.0f;
  float maxReceptionDiff = 0.0f;
  for (size_t i = 0; i < program.size(); ++i) {
    maxDiff = std::max(maxDiff,
                       std::fabs(oneBlock.output[i] -
                                 splitBlocks.output[i]));
    maxReceptionDiff = std::max(
        maxReceptionDiff,
        std::fabs(oneBlock.reception[i].desiredCarrierI -
                  splitBlocks.reception[i].desiredCarrierI));
    maxReceptionDiff = std::max(
        maxReceptionDiff,
        std::fabs(oneBlock.reception[i].desiredCarrierQ -
                  splitBlocks.reception[i].desiredCarrierQ));
    maxReceptionDiff = std::max(
        maxReceptionDiff,
        std::fabs(oneBlock.reception[i].skywaveI - splitBlocks.reception[i].skywaveI) +
            std::fabs(oneBlock.reception[i].skywaveQ - splitBlocks.reception[i].skywaveQ));
    maxReceptionDiff = std::max(
        maxReceptionDiff,
        std::fabs(oneBlock.reception[i].additiveRf -
                  splitBlocks.reception[i].additiveRf));
  }
  if (maxDiff > kTolerance) {
    fail("Broadcast source state depends on block boundaries; max diff=" +
         std::to_string(maxDiff));
  }
  if (maxReceptionDiff > kTolerance) {
    fail("Reception state depends on block boundaries; max diff=" +
         std::to_string(maxReceptionDiff));
  }
}

void expectSpeakerDerivedPhysicsRefreshesFromConfig() {
  Radio1938 radio;
  radio.init(1, kSampleRate, kBandwidthHz, 0.0f);

  auto& speaker = radio.speakerStage.speaker;
  const float originalDamping = speaker.effectiveMechanicalDampingNsPerMeter;
  const float originalBl = speaker.effectiveForceFactorBl;
  if (speaker.mechanicalDampingNsPerMeter != 0.0f ||
      speaker.forceFactorBl != 0.0f) {
    fail("Speaker init overwrote derived electromechanical config fields.");
  }

  speaker.mechanicalQ *= 0.5f;
  speaker.electricalQ *= 0.5f;
  radio.init(1, kSampleRate, kBandwidthHz, 0.0f);

  if (speaker.effectiveMechanicalDampingNsPerMeter <= originalDamping) {
    fail("Speaker mechanicalQ change did not increase effective damping.");
  }
  if (speaker.effectiveForceFactorBl <= originalBl) {
    fail("Speaker electricalQ change did not increase effective BL.");
  }
  if (speaker.mechanicalDampingNsPerMeter != 0.0f ||
      speaker.forceFactorBl != 0.0f) {
    fail("Speaker derived electromechanical values leaked into config.");
  }

  RadioSampleContext ctx{};
  RadioSpeakerNode::run(radio, 0.5f, ctx);
  radio.setIdentitySeed(radio.identity.seed);
  const float atRest = RadioSpeakerNode::run(radio, 0.0f, ctx);
  if (atRest != 0.0f || speaker.coneDisplacementMeters != 0.0f) {
    fail("Identity refresh left acoustic state after resetting the motor.");
  }
}

}  // namespace

int main() {
  expectBlockInvariantAmIngress();
  expectHotProgramLevelReachesAmIngress();
  expectTerminalObservationPreservesPlayback();
  expectReceptionProfileContract();
  expectReceptionSidecarShapesRf();
  expectStrongLocalReceptionIsTransparent();
  expectExternalNoiseFollowsFieldAndNoiseFigure();
  expectAtmosphericNoiseFollowsP372();
  expectReceiversRecoverFromLightningCrashes();
  expectEverydayReceptionProducesSlowCoherentFading();
  expectDelayedSkywaveFadesSelectively();
  expectIntermittentCarrierIsRareRfInsteadOfPermanentWhistle();
  expectPreviewCapsDetectorWork();
  expectPreviewUsesRequestedAudioBandwidthOnce();
  expectBroadcastSourceBypassIsTransparent();
  expectBroadcastCompressionTiming();
  expectBroadcastCompressionSlope();
  expectNetworkProgramLine();
  expectOperatorRidesToTheLimiter();
  expectBroadcastNonlinearityNearOnePercentThd();
  expectBroadcastNoiseIsBroadbandWithoutWhistle();
  expectPreviewSourceAndReceptionAreBlockInvariant();
  expectSpeakerDerivedPhysicsRefreshesFromConfig();
  return 0;
}
