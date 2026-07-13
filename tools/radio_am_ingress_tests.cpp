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
}

void expectReceptionSidecarShapesRf() {
  Radio1938 radio = makeIngressOnlyRadio();
  radio.iqInput.iqPhase = 0.5f * kRadioPi;
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
    const float magnitude = std::hypot(reception.desiredCarrierI,
                                       reception.desiredCarrierQ);
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

float measurePreviewProgramFilterRms(float frequencyHz) {
  Radio1938 radio;
  radio.init(1, kSampleRate, kBandwidthHz, 0.0f);
  RadioAmIngressConfig ingress;
  RadioPreviewConfig config;
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, config, kSampleRate);

  RadioPreviewSampleContext ctx{};
  double sumSq = 0.0;
  constexpr uint32_t kFrames = 48000u;
  constexpr uint32_t kAnalysisStart = kFrames / 2u;
  for (uint32_t i = 0; i < kFrames; ++i) {
    const float phase = kRadioTwoPi * frequencyHz *
                        (static_cast<float>(i) / kSampleRate);
    const float out =
        RadioPreviewProgramFilterNode::run(preview, std::sin(phase), ctx);
    if (i >= kAnalysisStart) {
      sumSq += static_cast<double>(out) * static_cast<double>(out);
    }
  }
  return static_cast<float>(
      std::sqrt(sumSq / static_cast<double>(kFrames - kAnalysisStart)));
}

void expectPreviewUsesRequestedAudioBandwidthOnce() {
  const float passbandRms = measurePreviewProgramFilterRms(500.0f);
  const float edgeRms = measurePreviewProgramFilterRms(kBandwidthHz);
  const float edgeRatio = edgeRms / std::max(passbandRms, 1e-9f);
  if (edgeRatio < 0.68f || edgeRatio > 0.74f) {
    fail("Preview audio-bandwidth edge is not a single -3 dB low-pass; ratio=" +
         std::to_string(edgeRatio));
  }
}

RadioAmIngressConfig makeIsolatedBroadcastSourceConfig() {
  RadioAmIngressConfig ingress;
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
  RadioAmIngressConfig ingress;
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
}

}  // namespace

int main() {
  expectBlockInvariantAmIngress();
  expectHotProgramLevelReachesAmIngress();
  expectReceptionProfileContract();
  expectReceptionSidecarShapesRf();
  expectStrongLocalReceptionIsTransparent();
  expectEverydayReceptionProducesSlowCoherentFading();
  expectIntermittentCarrierIsRareRfInsteadOfPermanentWhistle();
  expectPreviewCapsDetectorWork();
  expectPreviewUsesRequestedAudioBandwidthOnce();
  expectBroadcastSourceBypassIsTransparent();
  expectBroadcastCompressionTiming();
  expectBroadcastCompressionSlope();
  expectBroadcastNonlinearityNearOnePercentThd();
  expectBroadcastNoiseIsBroadbandWithoutWhistle();
  expectPreviewSourceAndReceptionAreBlockInvariant();
  expectSpeakerDerivedPhysicsRefreshesFromConfig();
  return 0;
}
