#include "radio.h"
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
  expectPreviewCapsDetectorWork();
  expectSpeakerDerivedPhysicsRefreshesFromConfig();
  return 0;
}
