#pragma once

#include <array>
#include <atomic>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "audiofilter/radio1938/radio_circuit_observation.h"
#include "radio_filter_mode.h"

// A receiver observation is paired with the output frames it produced. It
// contains physical signal measurements; visual scaling belongs to the UI.
struct RadioPlaybackObservation {
  RadioFilterMode mode = RadioFilterMode::Off;
  RadioCircuitObservation circuit;
  uint64_t generation = 0;
};

struct RadioVisualSegment {
  uint32_t offset = 0;
  uint32_t frames = 0;
  RadioPlaybackObservation observation;
};

struct RadioVisualBatch {
  std::vector<RadioVisualSegment> segments;

  void append(uint32_t offset, uint32_t frames,
              const RadioPlaybackObservation& observation) {
    if (frames) segments.push_back({offset, frames, observation});
  }
};

// One DSP writer and one or more presentation readers. Every field is atomic
// because a slot can be overwritten while a reader inspects it. The sequence
// check prevents mixing fields from two different observations.
class RadioVisualTimeline {
 public:
  void setEnabled(bool value) {
    if (enabled_.exchange(value, std::memory_order_acq_rel) != value)
      reset();
  }
  bool enabled() const { return enabled_.load(std::memory_order_acquire); }
  uint64_t generation() const { return generation_.load(std::memory_order_acquire); }
  void reset() { generation_.fetch_add(1, std::memory_order_acq_rel); }

  void publish(uint64_t generation, uint64_t first, uint64_t end,
               const RadioPlaybackObservation& observation,
               uint32_t minimumWindowFrames = 1) {
    assert(end > first && minimumWindowFrames > 0);
    const uint64_t frames = end - first;
    const bool merge = currentSerial_ != 0 && currentGeneration_ == generation &&
        currentEnd_ == first && currentFrames_ < minimumWindowFrames &&
        current_.mode == observation.mode &&
        current_.circuit.observed == observation.circuit.observed &&
        current_.circuit.supplyReferenceVolts ==
            observation.circuit.supplyReferenceVolts;
    if (!merge) {
      currentSerial_ = written_.load(std::memory_order_relaxed) + 1;
      currentGeneration_ = generation;
      currentFirst_ = first;
      currentFrames_ = frames;
      current_ = observation;
      for (std::size_t i = 0; i < kRadioCircuitProbeCount; ++i) {
        const auto& reading = observation.circuit.readings[i];
        means_[i] = reading.mean;
        deviations_[i] = double(reading.acRms) * reading.acRms * frames;
      }
    } else {
      const double previousFrames = static_cast<double>(currentFrames_);
      const double nextFrames = static_cast<double>(frames);
      const double totalFrames = previousFrames + nextFrames;
      for (std::size_t i = 0; i < kRadioCircuitProbeCount; ++i) {
        const auto& next = observation.circuit.readings[i];
        const double delta = double(next.mean) - means_[i];
        deviations_[i] += double(next.acRms) * next.acRms * nextFrames +
                          delta * delta * previousFrames * nextFrames / totalFrames;
        means_[i] += delta * nextFrames / totalFrames;
        const double acPower = std::max(0.0, deviations_[i] / totalFrames);
        auto& combined = current_.circuit.readings[i];
        combined.mean = static_cast<float>(means_[i]);
        combined.acRms = static_cast<float>(std::sqrt(acPower));
        combined.rms = static_cast<float>(std::sqrt(acPower + means_[i] * means_[i]));
      }
      currentFrames_ += frames;
    }
    currentEnd_ = end;
    Slot& slot = slots_[currentSerial_ % slots_.size()];
    slot.version.store(++mutationSequence_);  // odd: being written
    slot.serial.store(currentSerial_);
    slot.generation.store(generation);
    slot.first.store(currentFirst_);
    slot.end.store(end);
    slot.mode.store(static_cast<uint32_t>(current_.mode));
    slot.observed.store(current_.circuit.observed);
    slot.supplyReference.store(current_.circuit.supplyReferenceVolts);
    for (std::size_t i = 0; i < kRadioCircuitProbeCount; ++i) {
      slot.mean[i].store(current_.circuit.readings[i].mean);
      slot.rms[i].store(current_.circuit.readings[i].rms);
      slot.acRms[i].store(current_.circuit.readings[i].acRms);
    }
    slot.version.store(++mutationSequence_);  // even: complete snapshot
    if (!merge) written_.store(currentSerial_, std::memory_order_release);
  }

  bool at(uint64_t generation, uint64_t position,
          RadioPlaybackObservation& out) const {
    const uint64_t newest = written_.load(std::memory_order_acquire);
    const uint64_t oldest = newest > slots_.size() ? newest - slots_.size() + 1 : 1;
    for (uint64_t serial = newest; serial >= oldest && serial != 0; --serial) {
      const Slot& slot = slots_[serial % slots_.size()];
      const uint64_t version = slot.version.load();
      if (!version || (version & 1) || slot.serial.load() != serial) continue;
      if (slot.generation.load() != generation || slot.first.load() > position ||
          slot.end.load() <= position) continue;
      RadioPlaybackObservation candidate;
      candidate.generation = generation;
      candidate.mode = static_cast<RadioFilterMode>(slot.mode.load());
      candidate.circuit.observed = slot.observed.load();
      candidate.circuit.supplyReferenceVolts = slot.supplyReference.load();
      for (std::size_t i = 0; i < kRadioCircuitProbeCount; ++i) {
        candidate.circuit.readings[i].mean = slot.mean[i].load();
        candidate.circuit.readings[i].rms = slot.rms[i].load();
        candidate.circuit.readings[i].acRms = slot.acRms[i].load();
      }
      if (slot.version.load() == version) {
        out = candidate;
        return true;
      }
    }
    return false;
  }

 private:
  struct Slot {
    std::atomic<uint64_t> version{0}, serial{0}, generation{0}, first{0}, end{0};
    std::atomic<uint32_t> mode{0}, observed{0};
    std::atomic<float> supplyReference{0};
    std::array<std::atomic<float>, kRadioCircuitProbeCount> mean{};
    std::array<std::atomic<float>, kRadioCircuitProbeCount> rms{};
    std::array<std::atomic<float>, kRadioCircuitProbeCount> acRms{};
  };
  std::atomic<bool> enabled_{false};
  std::atomic<uint64_t> generation_{1}, written_{0};
  std::array<Slot, 512> slots_{};
  // The following state belongs exclusively to the DSP writer. Adjacent
  // short blocks are combined using parallel variance, keeping the history
  // bounded in time even when a source supplies one frame at a time.
  uint64_t mutationSequence_ = 0;
  uint64_t currentSerial_ = 0, currentGeneration_ = 0;
  uint64_t currentFirst_ = 0, currentEnd_ = 0, currentFrames_ = 0;
  RadioPlaybackObservation current_;
  std::array<double, kRadioCircuitProbeCount> means_{}, deviations_{};
};
