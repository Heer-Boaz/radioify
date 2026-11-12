#include "audio/pipeline_transition.h"
#include "audio/radio_playback.h"
#include "playback/video/ascii/asciiart.h"
#include "playback/video/gpu/gpu_runtime.h"
#include "tui/ui/radio_visualization_scene.h"
#include "tui/ui/radio_visualization_model.h"
#include "tui/ui/radio_visualization_frame_renderer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

namespace {
void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "radio_visualization_tests: " << message << '\n';
    std::exit(1);
  }
}

void timelineFollowsOutputAndReset() {
  RadioVisualTimeline timeline;
  RadioPlaybackObservation quiet, loud, found;
  quiet.mode = loud.mode = RadioFilterMode::Typical1930s;
  quiet.circuit.readings[0].rms = .2f;
  loud.circuit.readings[0].rms = .8f;
  const auto generation = timeline.generation();
  timeline.publish(generation, 100, 200, quiet);
  timeline.publish(generation, 200, 300, loud);
  require(!timeline.at(generation, 99, found), "pre-buffer data was shown");
  require(timeline.at(generation, 199, found) && found.circuit.readings[0].rms == .2f,
          "presented a future observation");
  require(timeline.at(generation, 200, found) && found.circuit.readings[0].rms == .8f,
          "did not advance at the audio boundary");
  require(!timeline.at(generation, 300, found), "extended a stale block");
  timeline.reset();
  require(!timeline.at(timeline.generation(), 200, found),
          "a seek reused the previous transport generation");
  for (uint64_t i = 0; i < 600; ++i)
    timeline.publish(timeline.generation(), i*10, i*10+10, loud);
  require(!timeline.at(timeline.generation(), 0, found) &&
              timeline.at(timeline.generation(), 5990, found),
          "bounded timeline did not discard its oldest slots");
  timeline.setEnabled(true);
  const auto enabledGeneration = timeline.generation();
  timeline.publish(enabledGeneration, 10, 20, quiet);
  timeline.setEnabled(false);
  timeline.setEnabled(true);
  require(timeline.generation() != enabledGeneration &&
              !timeline.at(timeline.generation(), 15, found),
          "reopening the view reused an old observation");
}

void playbackCursorDrainsDeviceTail() {
  RadioVisualTimeline timeline;
  const auto generation = timeline.generation();
  RadioPlaybackObservation quiet, loud, found;
  quiet.circuit.readings[0].rms = .2f;
  loud.circuit.readings[0].rms = .8f;
  timeline.publish(generation, 5199, 7599, quiet);
  timeline.publish(generation, 7599, 10000, loud);

  const auto playing = radioPlaybackCursor(10000, 4800, 48000, false,
                                           1000000, 9000000);
  require(playing.frame && *playing.frame == 5199 && !playing.outputFinished,
          "normal playback must retain the existing device-delay position");
  const auto tailStart = radioPlaybackCursor(10000, 4800, 48000, true,
                                             1000000, 1000000);
  require(tailStart.frame &&
              timeline.at(generation, *tailStart.frame, found) &&
              found.circuit.readings[0].rms == .2f,
          "EOF must retain the portion of audio still in the device");
  const auto tailMiddle = radioPlaybackCursor(10000, 4800, 48000, true,
                                              1000000, 1050000);
  require(tailMiddle.frame && *tailMiddle.frame == 7599 &&
              timeline.at(generation, *tailMiddle.frame, found) &&
              found.circuit.readings[0].rms == .8f,
          "the visual cursor must advance through the queued device tail");
  const auto tailEnd = radioPlaybackCursor(10000, 4800, 48000, true,
                                           1000000, 1100000);
  require(tailEnd.outputFinished && !tailEnd.frame,
          "a drained device must not repeat its final observation");

  const auto shortWaiting = radioPlaybackCursor(500, 4800, 48000, true,
                                                1000000, 1000000);
  const auto shortAudible = radioPlaybackCursor(500, 4800, 48000, true,
                                                1000000, 1090000);
  require(!shortWaiting.outputFinished && !shortWaiting.frame &&
              shortAudible.frame && *shortAudible.frame == 19,
          "a source shorter than device latency must wait for its first frame");
  require(radioPlaybackCursor(10000, 4800, 48000, true, 0, 1000000)
              .outputFinished &&
              radioPlaybackCursor(10000, 0, 48000, true, 1000000, 1000000)
                  .outputFinished &&
              radioPlaybackCursor(10000, 4800, 0, true, 1000000, 1000000)
                  .outputFinished,
          "EOF without a usable device clock must not present stale data");
}

void observationsRetainSmallModulation() {
  RadioCircuitAccumulator accumulator;
  const float low = 100.0f;
  const float high = 100.001f;
  for (int i = 0; i < 4096; ++i)
    accumulator.add(RadioCircuitProbe::DetectorVolts, (i & 1) ? high : low);
  const auto observation = accumulator.result(200.0f);
  const auto& reading = observation.readings[
      static_cast<std::size_t>(RadioCircuitProbe::DetectorVolts)];
  const float expected = (high - low) * .5f;
  require(std::fabs(reading.acRms - expected) < expected * .001f,
          "small AC modulation was lost when a large DC signal was averaged");
  RadioVisualTimeline timeline;
  RadioPlaybackObservation published, received;
  published.circuit = observation;
  timeline.publish(timeline.generation(), 10, 20, published);
  require(timeline.at(timeline.generation(), 15, received) &&
              received.circuit.readings[static_cast<std::size_t>(
                  RadioCircuitProbe::DetectorVolts)].acRms == reading.acRms,
          "timeline did not preserve the physical AC measurement");
}

void timelineAggregatesSmallAudioBlocks() {
  RadioVisualTimeline timeline;
  RadioCircuitAccumulator reference;
  const uint64_t generation = timeline.generation();
  const float low = 100.0f;
  const float high = 100.001f;
  const auto probe = RadioCircuitProbe::DetectorVolts;
  const auto index = static_cast<std::size_t>(probe);
  for (uint64_t frame = 0; frame < 12000; ++frame) {
    const float value = (frame & 1) ? high : low;
    RadioPlaybackObservation single;
    single.mode = RadioFilterMode::Typical1930s;
    single.circuit.observed = radioCircuitProbeBit(probe);
    single.circuit.readings[index].mean = value;
    single.circuit.readings[index].rms = value;
    if (frame < 480) reference.add(probe, value);
    timeline.publish(generation, frame, frame + 1, single, 480);
  }
  RadioPlaybackObservation first, last;
  require(timeline.at(generation, 0, first) &&
              timeline.at(generation, 11999, last),
          "single-frame input discarded audio still within the output buffer");
  const auto expected = reference.result(0).readings[index];
  require(std::fabs(first.circuit.readings[index].mean - expected.mean) < .00001f &&
              std::fabs(first.circuit.readings[index].acRms - expected.acRms) <
                  expected.acRms * .001f,
          "coalescing small blocks changed the measured signal");

  RadioPlaybackObservation philco;
  philco.mode = RadioFilterMode::Philco37116;
  timeline.publish(generation, 12000, 12001, philco, 480);
  require(timeline.at(generation, 11999, last) &&
              last.mode == RadioFilterMode::Typical1930s &&
              timeline.at(generation, 12000, last) &&
              last.mode == RadioFilterMode::Philco37116,
          "coalescing merged different receiver modes");
}

void timelineDoesNotMixConcurrentObservations() {
  RadioVisualTimeline timeline;
  const uint64_t generation = timeline.generation();
  std::atomic<uint64_t> published{0};
  std::atomic<bool> done{false};
  std::thread writer([&] {
    for (uint64_t i = 1; i <= 20000; ++i) {
      RadioPlaybackObservation observation;
      observation.circuit.readings[0].mean = static_cast<float>(i);
      observation.circuit.readings[0].rms = static_cast<float>(i + 1);
      observation.circuit.readings[0].acRms = static_cast<float>(i + 2);
      timeline.publish(generation, i, i + 1, observation);
      published.store(i, std::memory_order_release);
      if ((i & 63) == 0) std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
  });
  do {
    const uint64_t position = published.load(std::memory_order_acquire);
    RadioPlaybackObservation observed;
    if (position && timeline.at(generation, position, observed)) {
      const auto& reading = observed.circuit.readings[0];
      require(reading.mean == static_cast<float>(position) &&
                  reading.rms == static_cast<float>(position + 1) &&
                  reading.acRms == static_cast<float>(position + 2),
              "reader combined measurements from different DSP blocks");
    }
  } while (!done.load(std::memory_order_acquire));
  writer.join();
}

void timelineKeepsCoalescedSnapshotsConsistent() {
  RadioVisualTimeline timeline;
  const uint64_t generation = timeline.generation();
  std::atomic<uint64_t> published{0};
  std::atomic<bool> done{false};
  std::thread writer([&] {
    for (uint64_t i = 1; i <= 20000; ++i) {
      RadioPlaybackObservation observation;
      observation.circuit.observed =
          radioCircuitProbeBit(RadioCircuitProbe::AntennaEnvelopeVolts);
      observation.circuit.readings[0].mean = static_cast<float>(i);
      observation.circuit.readings[0].rms = static_cast<float>(i);
      timeline.publish(generation, i, i + 1, observation, 480);
      published.store(i, std::memory_order_release);
      if ((i & 63) == 0) std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
  });
  do {
    const uint64_t position = published.load(std::memory_order_acquire);
    RadioPlaybackObservation observed;
    if (position && timeline.at(generation, position, observed)) {
      const auto& reading = observed.circuit.readings[0];
      const float expectedRms = std::hypot(reading.mean, reading.acRms);
      require(std::fabs(reading.rms - expectedRms) < expectedRms * .00001f &&
                  std::fabs(reading.mean - static_cast<float>(position)) < 480,
              "reader observed a partially updated coalesced block");
    }
  } while (!done.load(std::memory_order_acquire));
  writer.join();
}

void presentationFollowsTheAudibleReceiver() {
  using namespace tui_radio_visualization;
  Levels typical;
  typical.mode = RadioFilterMode::Typical1930s;
  typical.observed = radioCircuitProbeBit(RadioCircuitProbe::DetectorVolts);
  const Presentation switching = present(&typical, RadioFilterMode::Philco37116,
                                         PlaybackPhase::Playing);
  require(switching.status == Status::Switching && switching.showCircuit,
          "a requested filter change hid the receiver still in the audio buffer");
  const Presentation switchingOff = present(&typical, RadioFilterMode::Off,
                                            PlaybackPhase::Paused);
  require(switchingOff.status == Status::Switching && switchingOff.showCircuit,
          "a paused filter change discarded the last audible receiver");
  const Presentation paused = present(&typical, RadioFilterMode::Typical1930s,
                                      PlaybackPhase::Paused);
  require(paused.status == Status::Paused && paused.showCircuit,
          "pause did not hold the observed receiver");
  const Presentation ended = present(&typical, RadioFilterMode::Typical1930s,
                                     PlaybackPhase::Finished);
  require(ended.status == Status::Finished && !ended.showCircuit &&
              present(nullptr, RadioFilterMode::Typical1930s,
                      PlaybackPhase::Finished).status == Status::Finished,
          "a finished track must not display its last reading as live activity");
  const Presentation replayed = present(&typical, RadioFilterMode::Typical1930s,
                                        PlaybackPhase::Playing);
  require(replayed.status == Status::Playing && replayed.showCircuit,
          "playback after EOF must show live circuit data again");
  Levels philco;
  philco.mode = RadioFilterMode::Philco37116;
  require(present(&philco, RadioFilterMode::Philco37116, PlaybackPhase::Playing).status ==
              Status::UnsupportedReceiver,
          "the audible Philco did not show its unsupported-view status");
  typical.observed = 0;
  require(present(&typical, RadioFilterMode::Typical1930s, PlaybackPhase::Playing).status ==
              Status::NoCircuitObservation,
          "a bypassed graph was displayed as a measured receiver");
}

void observationTracksEnabledStages() {
  Radio1938 radio;
  radio.init(1, 48000, 5500, .012f);
  RadioAmIngressConfig ingress;
  RadioPreviewPipeline preview;
  preview.initialize(radio, ingress, RadioPreviewConfig{}, 48000);
  RadioCircuitAccumulator accumulator;
  std::vector<float> samples(1024, .15f);
  preview.runBlock(radio, samples.data(), 1024, 1, &accumulator);
  const auto initial = accumulator.result(radio.powerSupply.mainReferenceVolts);
  require(initial.has(RadioCircuitProbe::AntennaEnvelopeVolts) &&
              initial.has(RadioCircuitProbe::DetectorVolts) &&
              initial.has(RadioCircuitProbe::AudioPlateSwingVolts) &&
              initial.has(RadioCircuitProbe::SpeakerTerminalVolts),
          "active stages did not publish their physical measurements");

  radio.graph.setEnabled(PassId::FrontEnd, false);
  radio.graph.setEnabled(PassId::Demod, false);
  radio.graph.setEnabled(PassId::ReceiverCircuit, false);
  radio.graph.setEnabled(PassId::Power, false);
  std::fill(samples.begin(), samples.end(), .15f);
  preview.runBlock(radio, samples.data(), 1024, 1, &accumulator);
  const auto bypassed = accumulator.result(radio.powerSupply.mainReferenceVolts);
  require(!bypassed.has(RadioCircuitProbe::AntennaEnvelopeVolts) &&
              !bypassed.has(RadioCircuitProbe::DetectorVolts) &&
              !bypassed.has(RadioCircuitProbe::AudioPlateSwingVolts) &&
              !bypassed.has(RadioCircuitProbe::SpeakerTerminalVolts) &&
              !bypassed.has(RadioCircuitProbe::SpeakerCoilAmps) &&
              !bypassed.has(RadioCircuitProbe::SupplyVolts),
          "a bypassed stage reported stale measurements from an earlier block");

  radio.graph.bypass = true;
  std::fill(samples.begin(), samples.end(), .15f);
  preview.runBlock(radio, samples.data(), 1024, 1, &accumulator);
  require(accumulator.result(radio.powerSupply.mainReferenceVolts).observed == 0,
          "a fully bypassed receiver retained old observations");
}

void observationPreservesAudioAndTracksSignal() {
  RadioPlaybackFilter measured, plain;
  RadioPlaybackFilterConfig config;
  config.initialMode = RadioFilterMode::Typical1930s;
  config.outputChannels = 2;
  measured.initialize(config);
  plain.initialize(config);
  AudioPipelineTransition measuredTransition, plainTransition;
  constexpr int frames = 1024;
  tui_radio_visualization::Levels quiet, tone;
  for (int block = 0; block < 18; ++block) {
    std::vector<float> a(frames * 2), b;
    for (int i = 0; i < frames; ++i) {
      const float sample = block < 9 ? 0.0f :
          .6f * std::sin(float((block*frames+i)*6.283185307179586*1000/48000));
      a[2*i] = a[2*i+1] = sample;
    }
    b = a;
    RadioVisualBatch batch;
    measured.process(a.data(), frames, 2, measuredTransition, &batch);
    plain.process(b.data(), frames, 2, plainTransition);
    require(a == b, "collecting observations changed the audio");
    require(batch.segments.size() == 1 && batch.segments[0].frames == frames &&
                batch.segments[0].observation.mode == RadioFilterMode::Typical1930s,
            "the receiver did not publish a complete typical-radio block");
    if (block == 8) quiet = tui_radio_visualization::project(batch.segments[0].observation);
    if (block == 17) tone = tui_radio_visualization::project(batch.segments[0].observation);
  }
  for (std::size_t i = 0; i < kRadioCircuitProbeCount; ++i) {
    require(std::isfinite(tone.values[i]) && tone.values[i] >= 0 &&
                tone.values[i] <= 1, "stage intensity was not bounded");
    std::cout << "stage " << i << " quiet=" << quiet.values[i]
              << " tone=" << tone.values[i] << '\n';
  }
  const auto output = static_cast<std::size_t>(RadioCircuitProbe::SpeakerTerminalVolts);
  const auto speaker = static_cast<std::size_t>(RadioCircuitProbe::SpeakerCoilAmps);
  const auto antenna = static_cast<std::size_t>(RadioCircuitProbe::AntennaEnvelopeVolts);
  const auto converter = static_cast<std::size_t>(RadioCircuitProbe::ConverterEnvelopeAmps);
  const auto intermediate = static_cast<std::size_t>(RadioCircuitProbe::IfEnvelopeVolts);
  const auto detector = static_cast<std::size_t>(RadioCircuitProbe::DetectorVolts);
  require(tone.values[antenna] > quiet.values[antenna] + .05f &&
              tone.values[converter] > quiet.values[converter] + .05f &&
              tone.values[intermediate] > quiet.values[intermediate] + .05f &&
              quiet.values[antenna] < .8f && quiet.values[converter] < .8f &&
              quiet.values[intermediate] < .8f,
          "RF stages did not show the carrier and its modulation separately");
  require(tone.values[detector] > quiet.values[detector] + .1f &&
              tone.values[detector] < .98f,
          "detector activity should respond without saturating at the reference tone");
  require(tone.values[output] > quiet.values[output] + .05f &&
              tone.values[speaker] > quiet.values[speaker] + .05f,
          "output and speaker did not respond to a modulated carrier");

  // The visualization follows the receiver that actually produced each
  // portion of a transition, including the subsequent unfiltered segment.
  for (RadioFilterMode next : {RadioFilterMode::Philco37116,
                               RadioFilterMode::Off}) {
    measured.cycleMode();
    bool sawNext = false;
    for (int block = 0; block < 4 && !sawNext; ++block) {
      std::vector<float> samples(frames * 2, .1f);
      RadioVisualBatch batch;
      measured.process(samples.data(), frames, 2, measuredTransition, &batch);
      uint32_t offset = 0;
      for (std::size_t i = 0; i < batch.segments.size(); ++i) {
        const auto& segment = batch.segments[i];
        require(segment.offset == offset && segment.frames > 0,
                "receiver transition left a gap in the visual timeline");
        offset += segment.frames;
        sawNext = sawNext || segment.observation.mode == next;
      }
      require(offset == frames, "receiver transition did not cover its output block");
    }
    require(sawNext, "receiver transition published the wrong visualization mode");
  }

  RadioPlaybackFilter largeFilter;
  largeFilter.initialize(config);
  AudioPipelineTransition largeTransition;
  std::vector<float> largeSamples(10240 * 2, .1f);
  RadioVisualBatch largeBatch;
  largeFilter.process(largeSamples.data(), 10240, 2, largeTransition, &largeBatch);
  uint32_t covered = 0;
  for (const auto& segment : largeBatch.segments) {
    require(segment.offset == covered, "large visual batch contains a gap");
    covered += segment.frames;
  }
  require(covered == 10240 && largeBatch.segments.size() >= 5,
          "visualization silently truncated a large processing batch");
}

void sceneReflectsTheObservedStages() {
  tui_radio_visualization::Levels low, high;
  low.mode = high.mode = RadioFilterMode::Typical1930s;
  low.observed = high.observed = (1u << kRadioCircuitProbeCount) - 1;
  high.values.fill(.8f);
  std::vector<uint8_t> lowPixels, highPixels;
  tui_radio_visualization::drawRadio(lowPixels, &low);
  tui_radio_visualization::drawRadio(highPixels, &high);
  require(lowPixels.size() == highPixels.size() && lowPixels != highPixels,
          "the chassis did not respond to its stage observations");

  // The heater remains faint at idle, but a quiet output stage must not keep
  // a fixed full-bright filament. Driving that stage must not brighten the
  // independent converter or the speaker motor.
  tui_radio_visualization::Levels outputOnly = low;
  outputOnly.values[static_cast<std::size_t>(
      RadioCircuitProbe::SpeakerTerminalVolts)] = 1.0f;
  std::vector<uint8_t> outputPixels;
  tui_radio_visualization::drawRadio(outputPixels, &outputOnly);
  const auto redAt = [](const std::vector<uint8_t>& pixels, int x, int y) {
    return pixels[(static_cast<std::size_t>(y) *
                       tui_radio_visualization::kWidth + x) * 4];
  };
  require(redAt(lowPixels, 452, 280) < 80 &&
              redAt(outputPixels, 452, 280) > redAt(lowPixels, 452, 280) + 100,
          "the output tube did not visibly dim at zero signal");
  require(redAt(lowPixels, 191, 167) == redAt(outputPixels, 191, 167) &&
              redAt(lowPixels, 570, 167) == redAt(outputPixels, 570, 167),
          "the output signal lit an unrelated component");
  AsciiArtRenderer renderer;
  AsciiArt art;
  require(renderer.renderRgbaExact(highPixels.data(), tui_radio_visualization::kWidth,
                                  tui_radio_visualization::kHeight, 100, 31,
                                  art, true) && art.cells.size() == 3100,
          "the existing ASCII renderer could not render the chassis");
}

void sceneRasterReusesOnlyUnchangedInputs() {
  tui_radio_visualization::SceneRaster raster;
  std::vector<uint8_t> expected;
  require(raster.update(nullptr), "an unobserved chassis must be rasterized once");
  tui_radio_visualization::drawRadio(expected, nullptr);
  require(raster.pixels() == expected && !raster.update(nullptr),
          "an unchanged unobserved chassis must reuse the exact raster");

  tui_radio_visualization::Levels levels;
  levels.mode = RadioFilterMode::Typical1930s;
  levels.generation = 1;
  levels.observed = (1u << kRadioCircuitProbeCount) - 1;
  levels.values.fill(.2f);
  require(raster.update(&levels), "new circuit readings must redraw the chassis");
  tui_radio_visualization::drawRadio(expected, &levels);
  require(raster.pixels() == expected && !raster.update(&levels),
          "unchanged readings must reuse the exact raster");

  levels.generation = 2;
  require(!raster.update(&levels),
          "a new timeline generation with the same scene must reuse pixels");
  levels.values[static_cast<std::size_t>(
      RadioCircuitProbe::SpeakerTerminalVolts)] = .9f;
  require(raster.update(&levels), "changed stage activity must redraw the chassis");
  tui_radio_visualization::drawRadio(expected, &levels);
  require(raster.pixels() == expected,
          "the cached raster must reflect the changed stage exactly");

  levels.observed &= ~radioCircuitProbeBit(RadioCircuitProbe::SpeakerTerminalVolts);
  require(raster.update(&levels), "losing a probe must redraw the affected stage");
  tui_radio_visualization::drawRadio(expected, &levels);
  require(raster.pixels() == expected, "probe availability must affect the raster");
  require(raster.update(nullptr), "losing the observation must clear the circuit");
  tui_radio_visualization::drawRadio(expected, nullptr);
  require(raster.pixels() == expected && !raster.update(nullptr),
          "returning to an unobserved chassis must not retain stale activity");
}

void rendererEventuallyPresentsVisibleCells() {
  GpuRuntime gpu;
  const bool hasGpu = gpu.device() != nullptr;
  tui_radio_visualization::FrameRenderer renderer(gpu);
  tui_radio_visualization::Levels levels;
  levels.mode = RadioFilterMode::Typical1930s;
  levels.observed = (1u << kRadioCircuitProbeCount) - 1;
  levels.values.fill(.7f);

  // Exercise the real view, including its asynchronous GPU readback. Merely
  // checking the title or status would miss an indefinitely blank picture.
  for (uint64_t generation : {1u, 2u}) {
    levels.generation = generation;
    bool visible = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !visible) {
      const AsciiArt* art = renderer.render(&levels, generation, 100, 31);
      visible = art && std::any_of(art->cells.begin(), art->cells.end(),
          [](const AsciiArt::AsciiCell& cell) {
            return cell.ch > 0x2800 && cell.ch <= 0x28ff;
          });
      if (!visible) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(visible, "inside-radio view remained blank after its readback");
  }
  bool unobservedVisible = false;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline && !unobservedVisible) {
    const AsciiArt* art = renderer.render(nullptr, 3, 36, 11);
    unobservedVisible = art && std::any_of(art->cells.begin(), art->cells.end(),
        [](const AsciiArt::AsciiCell& cell) {
          return cell.ch > 0x2800 && cell.ch <= 0x28ff;
        });
    if (!unobservedVisible)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(unobservedVisible,
          "the unchanged chassis must still complete asynchronous readback");
  std::cout << (hasGpu ? "GPU" : "CPU")
            << " receiver rendering produced visible cells\n";
}

void independentSurfacesRenderAtTheirOwnSizes() {
  GpuRuntime gpu;
  tui_radio_visualization::FrameRenderer mainRenderer(gpu);
  tui_radio_visualization::FrameRenderer pipRenderer(gpu);
  tui_radio_visualization::Levels levels;
  levels.mode = RadioFilterMode::Typical1930s;
  levels.observed = (1u << kRadioCircuitProbeCount) - 1;
  levels.values.fill(.7f);
  levels.generation = 7;

  // Both surfaces share a GPU runtime but own independent readback history.
  // Resizing PiP must not invalidate the main window's frame or vice versa.
  for (const auto& size : {std::pair{36, 11}, std::pair{22, 7}}) {
    bool mainVisible = false;
    bool pipVisible = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline &&
           !(mainVisible && pipVisible)) {
      const auto visible = [](const AsciiArt* art, int width, int height) {
        return art && art->width == width && art->height == height &&
               std::any_of(art->cells.begin(), art->cells.end(),
                           [](const AsciiArt::AsciiCell& cell) {
                             return cell.ch > 0x2800 && cell.ch <= 0x28ff;
                           });
      };
      mainVisible = visible(mainRenderer.render(&levels, levels.generation,
                                               100, 31), 100, 31);
      pipVisible = visible(pipRenderer.render(&levels, levels.generation,
                                             size.first, size.second),
                           size.first, size.second);
      if (!(mainVisible && pipVisible))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(mainVisible && pipVisible,
            "independent main and PiP frames did not both become visible");
  }
}
}  // namespace

int main() {
  timelineFollowsOutputAndReset();
  playbackCursorDrainsDeviceTail();
  observationsRetainSmallModulation();
  timelineAggregatesSmallAudioBlocks();
  timelineDoesNotMixConcurrentObservations();
  timelineKeepsCoalescedSnapshotsConsistent();
  presentationFollowsTheAudibleReceiver();
  observationTracksEnabledStages();
  observationPreservesAudioAndTracksSignal();
  sceneReflectsTheObservedStages();
  sceneRasterReusesOnlyUnchangedInputs();
  rendererEventuallyPresentsVisibleCells();
  independentSurfacesRenderAtTheirOwnSizes();
}
