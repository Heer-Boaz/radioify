#include "radio.h"
#include "audio/pipeline_transition.h"
#include "audio/radio_playback.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void fail(const char* message) {
  std::cerr << "radio_receiver_profile_tests: " << message << '\n';
  std::exit(1);
}

void expectNear(float actual, float expected, float tolerance,
                const char* message) {
  if (!std::isfinite(actual) || std::fabs(actual - expected) > tolerance) {
    std::cerr << "radio_receiver_profile_tests: " << message
              << " expected=" << expected << " actual=" << actual << '\n';
    std::exit(1);
  }
}

void expectProfileParserContract() {
  if (kDefaultRadioReceiverProfile != RadioReceiverProfile::Typical1930s) {
    fail("typical-1930s is not the default receiver profile");
  }

  RadioReceiverProfile parsed = RadioReceiverProfile::Philco37116;
  if (!parseRadioReceiverProfile("typical-1930s", parsed) ||
      parsed != RadioReceiverProfile::Typical1930s ||
      radioReceiverProfileName(parsed) != "typical-1930s") {
    fail("typical-1930s does not round-trip");
  }
  if (!parseRadioReceiverProfile("philco-37-116", parsed) ||
      parsed != RadioReceiverProfile::Philco37116 ||
      radioReceiverProfileName(parsed) != "philco-37-116") {
    fail("philco-37-116 does not round-trip");
  }
  if (parseRadioReceiverProfile("generic-old-radio", parsed)) {
    fail("receiver parser accepted an undocumented profile");
  }
}

void expectTypicalReceiverTopology() {
  Radio1938 radio;
  if (radio.receiverProfile != RadioReceiverProfile::Typical1930s ||
      radio.frontEnd.secondTunedCircuitEnabled ||
      radio.power.topology !=
          Radio1938::PowerNodeState::Topology::CapCoupledSingleEnded) {
    fail("default receiver is not the single-ended mass-market topology");
  }
  expectNear(radio.receiverCircuit.volumeControlResistanceOhms, 500000.0f,
             1.0f, "38-12 volume-control value");
  expectNear(radio.power.outputLoadResistanceOhms, 3.5f, 0.01f,
             "BO-1 voice-coil load");
  expectNear(radio.power.outputTransformerPrimaryResistanceOhms, 450.0f,
             0.1f, "BO-1 transformer primary resistance");
  if (radio.speakerStage.speaker.suspensionHz < 180.0f ||
      radio.speakerStage.speaker.suspensionHz > 250.0f) {
    fail("small speaker left the Knowles period natural-frequency prior");
  }

  radio.init(1, 48000.0f, 5000.0f, 0.012f);
  if (radio.power.nominalOutputPowerWatts < 1.5f ||
      radio.power.nominalOutputPowerWatts > 2.5f) {
    fail("typical receiver did not initialize near its documented 2 W output");
  }
  if (!std::isfinite(radio.output.digitalReferenceSpeakerVoltsPeak) ||
      radio.output.digitalReferenceSpeakerVoltsPeak <= 0.0f) {
    fail("typical receiver has no finite speaker reference");
  }
}

void expectInitializedProfileSwitchRebuildsPhysicalModel() {
  Radio1938 radio;
  radio.init(1, 48000.0f, 5000.0f, 0.012f);
  radio.applyReceiverProfile(RadioReceiverProfile::Philco37116);
  if (radio.receiverProfile != RadioReceiverProfile::Philco37116 ||
      !radio.frontEnd.secondTunedCircuitEnabled ||
      radio.power.topology !=
          Radio1938::PowerNodeState::Topology::TransformerCoupledPushPull ||
      radio.power.nominalOutputPowerWatts < 12.0f ||
      radio.power.nominalOutputPowerWatts > 18.0f) {
    fail("initialized switch did not rebuild the Philco physical model");
  }

  radio.applyReceiverProfile(RadioReceiverProfile::Typical1930s);
  if (radio.receiverProfile != RadioReceiverProfile::Typical1930s ||
      radio.frontEnd.secondTunedCircuitEnabled ||
      radio.power.topology !=
          Radio1938::PowerNodeState::Topology::CapCoupledSingleEnded ||
      radio.power.nominalOutputPowerWatts < 1.5f ||
      radio.power.nominalOutputPowerWatts > 2.5f) {
    fail("switching back did not restore the typical physical model");
  }
}

// A profile reached through the other profile must be the same receiver as a
// freshly built one: every circuit value either profile sets is set by both.
std::vector<float> renderProfile(RadioReceiverProfile first, RadioReceiverProfile target) {
  Radio1938 radio;
  radio.applyReceiverProfile(first);
  radio.init(1, 48000.0f, 5000.0f, 0.0f);
  if (first != target) radio.applyReceiverProfile(target);
  radio.warmInputCarrier(0.12f);
  std::vector<float> input(9600), output(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    const double t = double(i) / 48000.0;
    input[i] = float(0.4 * std::sin(2 * 3.14159265358979 * 1000 * t) +
                     0.2 * std::sin(2 * 3.14159265358979 * 4000 * t));
  }
  radio.processAmAudio(input.data(), output.data(), uint32_t(input.size()), 0.12f, 0.85f);
  return output;
}

void expectProfileSwitchMatchesFreshReceiver() {
  for (auto [other, target] :
       {std::pair{RadioReceiverProfile::Philco37116, RadioReceiverProfile::Typical1930s},
        std::pair{RadioReceiverProfile::Typical1930s, RadioReceiverProfile::Philco37116}}) {
    if (renderProfile(target, target) != renderProfile(other, target)) {
      fail("a receiver switched from the other profile differs from a fresh one");
    }
  }
}

void expectPlaybackFilterOwnsReceiverSwitching() {
  RadioPlaybackFilter filter;
  RadioPlaybackFilterConfig config;
  config.sampleRate = 48000;
  config.outputChannels = 2;
  config.initialMode = RadioFilterMode::Off;
  filter.initialize(config);

  filter.cycleMode();
  filter.cycleMode();
  if (filter.mode() != RadioFilterMode::Philco37116) {
    fail("paused mode changes did not retain the latest receiver request");
  }

  AudioPipelineTransition pipelineTransition;
  audioPipelineTransitionReset(pipelineTransition);
  std::vector<float> samples(128, 0.05f);
  filter.process(samples.data(), 64, 2, pipelineTransition);
  for (float sample : samples) {
    if (!std::isfinite(sample)) {
      fail("playback owner produced a non-finite receiver transition");
    }
  }

  filter.requestReset();
  filter.requestReset();
  filter.process(samples.data(), 64, 2, pipelineTransition);

  filter.cycleMode();
  const uint32_t transitionFrames = audioPipelineTransitionFrames(48000);
  samples.assign(static_cast<size_t>(transitionFrames) * 2u, 0.05f);
  filter.process(samples.data(), transitionFrames, 2, pipelineTransition);
  samples.assign(128, 0.05f);
  filter.process(samples.data(), 64, 2, pipelineTransition);
  for (float sample : samples) {
    expectNear(sample, 0.05f, 1e-6f,
               "disabled playback owner must preserve dry audio");
  }
}

float runPlaybackPeak(std::vector<float> samples, RadioFilterMode mode) {
  constexpr uint32_t kSampleRate = 48000u;
  constexpr uint32_t kBlockFrames = 2048u;
  RadioPlaybackFilter filter;
  RadioPlaybackFilterConfig config;
  config.sampleRate = kSampleRate;
  config.outputChannels = 1u;
  config.bandwidthHz = 5500.0f;
  config.noise = 0.012f;
  config.initialMode = mode;
  filter.initialize(config);

  AudioPipelineTransition pipelineTransition;
  audioPipelineTransitionReset(pipelineTransition);
  const uint32_t totalFrames = static_cast<uint32_t>(samples.size());
  for (uint32_t frame = 0; frame < totalFrames; frame += kBlockFrames) {
    const uint32_t frames = std::min(kBlockFrames, totalFrames - frame);
    filter.process(samples.data() + frame, frames, 1u,
                   pipelineTransition);
  }
  float peak = 0.0f;
  for (float sample : samples) {
    if (!std::isfinite(sample)) fail("non-finite acoustic playback output");
    peak = std::max(peak, std::fabs(sample));
  }
  return peak;
}

void expectProgramHeadroom(const char* name, std::vector<float> program,
                          RadioFilterMode mode, float minimumPeak) {
  const float peak = runPlaybackPeak(std::move(program), mode);
  std::cout << "playback_headroom," << static_cast<int>(mode) << ',' << name
            << ',' << peak << '\n';
  if (peak > 0.95f) {
    std::cerr << "radio_receiver_profile_tests: " << name
              << " exhausted digital headroom; peak=" << peak << '\n';
    std::exit(1);
  }
  if (peak < minimumPeak) {
    fail("playback headroom fix attenuated the profile excessively");
  }
}

void expectPlaybackKeepsAcousticPeakHeadroom() {
  constexpr uint32_t kSampleRate = 48000u;
  constexpr uint32_t kFrames = kSampleRate / 2u;
  constexpr float kPi = 3.14159265358979323846f;

  std::vector<float> denseProgram(kFrames);
  for (uint32_t frame = 0; frame < kFrames; ++frame) {
    const float seconds = static_cast<float>(frame) / kSampleRate;
    const float program =
        0.56f * std::sin(2.0f * kPi * 180.0f * seconds) +
        0.29f * std::sin(2.0f * kPi * 830.0f * seconds + 0.7f) +
        0.15f * std::sin(2.0f * kPi * 2700.0f * seconds + 1.1f);
    denseProgram[frame] = std::clamp(program, -0.98f, 0.98f);
  }

  for (const auto mode : {RadioFilterMode::Typical1930s,
                          RadioFilterMode::Philco37116}) {
    float strongestPeak = 0.0f;
    // Cover both the bass resonance and the newly evidenced upper-mid peaks.
    // A 200 Hz minimum level would demand the former bass-heavy sound again.
    for (float hz : {125.0f, 200.0f, 315.0f, 500.0f, 1000.0f,
                     1600.0f, 2000.0f, 2500.0f, 3150.0f, 4000.0f}) {
      std::vector<float> sine(kFrames);
      for (uint32_t frame = 0; frame < kFrames; ++frame) {
        sine[frame] = 0.98f * std::cos(2.0f * kPi * hz *
                                      static_cast<float>(frame) / kSampleRate);
      }
      const float peak = runPlaybackPeak(std::move(sine), mode);
      std::cout << "playback_headroom," << static_cast<int>(mode) << ','
                << hz << ',' << peak << '\n';
      if (peak > 0.95f) fail("acoustic response peak exhausted digital headroom");
      strongestPeak = std::max(strongestPeak, peak);
    }
    // Steady-state peak: the receiver starts settled on the carrier, so no
    // turn-on AVC transient inflates it (that transient once exceeded 0.55).
    if (strongestPeak < 0.50f) fail("acoustic profile attenuated excessively");
    expectProgramHeadroom("dense programme", denseProgram, mode, 0.20f);
  }
}

struct NoiseProbe {
  std::vector<float> silence;
  double toneRms = 0.0;
};

enum class NoiseSource { None, External, Post, Hum, Broadcast, All };

const char* noiseSourceName(NoiseSource source) {
  switch (source) {
    case NoiseSource::None: return "residual";
    case NoiseSource::External: return "external";
    case NoiseSource::Post: return "post";
    case NoiseSource::Hum: return "hum";
    case NoiseSource::Broadcast: return "broadcast";
    case NoiseSource::All: return "all";
  }
  std::abort();
}

NoiseProbe measurePlaybackNoise(RadioReceiverProfile profile,
                                NoiseSource source,
                                RadioReceptionProfile reception =
                                    RadioReceptionProfile::Everyday1938) {
  constexpr uint32_t kRate = 48000;
  constexpr uint32_t kFrames = kRate * 2;
  constexpr double kTwoPi = 6.28318530717958647693;
  Radio1938 radio;
  radio.applyReceiverProfile(profile);
  const bool all = source == NoiseSource::All;
  if (!all && source != NoiseSource::Post) radio.globals.postNoiseMix = 0.0f;
  if (!all && source != NoiseSource::Hum) radio.noiseConfig.enableHumTone = false;
  radio.init(1, kRate, 5500.0f, 0.012f);
  RadioAmIngressConfig ingress;
  ingress.reception = radioReceptionConfigForProfile(reception);
  ingress.reception.externalNoiseEnabled = all || source == NoiseSource::External;
  ingress.broadcastSource.noiseEnabled = all || source == NoiseSource::Broadcast;
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, RadioPreviewConfig{}, kRate);
  std::vector<float> samples(kFrames, 0.0f);
  for (uint32_t i = kRate; i < kFrames; ++i) {
    samples[i] = static_cast<float>(0.5 * std::sin(kTwoPi * 1000.0 * i / kRate));
  }
  for (uint32_t offset = 0; offset < kFrames; offset += 2048) {
    preview.runBlock(radio, samples.data() + offset,
                     std::min(2048u, kFrames - offset), 1);
  }
  NoiseProbe probe;
  probe.silence.assign(samples.begin() + kRate / 2, samples.begin() + kRate);
  double re = 0.0;
  double im = 0.0;
  for (uint32_t i = kFrames - kRate / 2; i < kFrames; ++i) {
    if (!std::isfinite(samples[i])) fail("non-finite noise probe output");
    re += samples[i] * std::cos(kTwoPi * 1000.0 * i / kRate);
    im += samples[i] * std::sin(kTwoPi * 1000.0 * i / kRate);
  }
  probe.toneRms = std::sqrt(2.0 * (re * re + im * im)) / (kRate / 2);
  return probe;
}

double probeRms(const std::vector<float>& samples,
                const std::vector<float>* reference = nullptr) {
  double sum = 0.0;
  double sumSq = 0.0;
  for (size_t i = 0; i < samples.size(); ++i) {
    const double y = samples[i] - (reference ? (*reference)[i] : 0.0f);
    if (!std::isfinite(y)) fail("non-finite noise probe sample");
    sum += y;
    sumSq += y * y;
  }
  const double mean = sum / samples.size();
  return std::sqrt(std::max(0.0, sumSq / samples.size() - mean * mean));
}

void reportPlaybackNoise() {
  std::cout << "profile,source,silent_carrier_rms,tone_rms,snr_db,added_rms\n";
  for (auto profile : {RadioReceiverProfile::Typical1930s,
                       RadioReceiverProfile::Philco37116}) {
    const auto baseline = measurePlaybackNoise(profile, NoiseSource::None);
    for (auto source : {NoiseSource::None, NoiseSource::External, NoiseSource::Post,
                        NoiseSource::Hum, NoiseSource::Broadcast, NoiseSource::All}) {
      const auto probe = source == NoiseSource::None
                             ? baseline : measurePlaybackNoise(profile, source);
      const double noise = probeRms(probe.silence);
      std::cout << radioReceiverProfileName(profile) << ','
                << noiseSourceName(source) << ',' << noise << ','
                << probe.toneRms << ',' << 20.0 * std::log10(probe.toneRms / noise)
                << ',' << probeRms(probe.silence, &baseline.silence) << '\n';
    }
  }
}

void expectPlaybackNoiseBalance() {
  for (auto profile : {RadioReceiverProfile::Typical1930s,
                       RadioReceiverProfile::Philco37116}) {
    const auto measure = [&](RadioReceptionProfile reception) {
      // Only the noise this source adds: the deterministic residual of the
      // silent carrier is subtracted sample by sample.
      const auto probe = measurePlaybackNoise(profile, NoiseSource::External, reception);
      const auto residual = measurePlaybackNoise(profile, NoiseSource::None, reception);
      const double noise = probeRms(probe.silence, &residual.silence);
      const double snr = 20.0 * std::log10(probe.toneRms / noise);
      std::cout << "playback_noise," << radioReceiverProfileName(profile) << ','
                << radioReceptionProfileName(reception) << ',' << noise << ','
                << probe.toneRms << ',' << snr << '\n';
      if (!std::isfinite(snr)) fail("non-finite playback noise ratio");
      return snr;
    };
    // Signal-relative, so makeup gain cannot hide noise. Noise enters at the
    // antenna with the carrier and AVC holds the carrier, so a stronger field
    // must improve the audio ratio by the field ratio itself, whatever the
    // receiver's gain.
    const double everyday = measure(RadioReceptionProfile::Everyday1938);
    const double strong = measure(RadioReceptionProfile::StrongLocal);
    const auto ratioDb = [](RadioReceptionProfile reception) {
      return radioReceptionCarrierToManMadeNoiseDbHz(
          radioReceptionConfigForProfile(reception), 1e6);
    };
    const double expected = ratioDb(RadioReceptionProfile::StrongLocal) -
                            ratioDb(RadioReceptionProfile::Everyday1938);
    if (std::fabs((strong - everyday) - expected) > 1.5)
      fail("audio noise does not follow the antenna carrier-to-noise ratio");
    // FCC 1939: at the primary-service field the usual noise is no longer
    // objectionable, i.e. weaker than a co-channel programme 26 dB down.
    // The probe tone is 42.5% modulation, not full programme, so this is
    // conservative.
    if (everyday < 26.0) fail("noise at the FCC primary-service field is objectionable");
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--noise-report") {
    reportPlaybackNoise();
    return 0;
  }
  expectProfileParserContract();
  expectTypicalReceiverTopology();
  expectInitializedProfileSwitchRebuildsPhysicalModel();
  expectProfileSwitchMatchesFreshReceiver();
  expectPlaybackFilterOwnsReceiverSwitching();
  expectPlaybackKeepsAcousticPeakHeadroom();
  expectPlaybackNoiseBalance();
  return 0;
}
