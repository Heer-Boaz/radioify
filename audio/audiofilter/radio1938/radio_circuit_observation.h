#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Measurements taken at actual receiver stages, in the units in their names.
// The field coil and speaker motor are solved together in the power stage.
enum class RadioCircuitProbe : std::size_t {
  AntennaEnvelopeVolts,
  ConverterEnvelopeAmps,
  IfEnvelopeVolts,
  DetectorVolts,
  AudioPlateSwingVolts,
  SpeakerTerminalVolts,
  SpeakerCoilAmps,
  SupplyVolts,
  Count,
};

constexpr std::size_t kRadioCircuitProbeCount =
    static_cast<std::size_t>(RadioCircuitProbe::Count);

constexpr uint32_t radioCircuitProbeBit(RadioCircuitProbe probe) {
  return uint32_t{1} << static_cast<std::size_t>(probe);
}

struct RadioCircuitObservation {
  struct Reading {
    float mean = 0.0f;
    float rms = 0.0f;
    float acRms = 0.0f;
  };
  std::array<Reading, kRadioCircuitProbeCount> readings{};
  uint32_t observed = 0;
  float supplyReferenceVolts = 0.0f;

  bool has(RadioCircuitProbe probe) const {
    return (observed & radioCircuitProbeBit(probe)) != 0;
  }
};

// Caller-owned and used only on the processing thread. A probe is recorded
// only when its physical stage ran; a bypassed stage is therefore absent,
// rather than accidentally reporting its last retained state.
class RadioCircuitAccumulator {
 public:
  void reset() { measurements_ = {}; }

  void add(RadioCircuitProbe probe, float value) {
    Measurement& measurement = measurements_[static_cast<std::size_t>(probe)];
    ++measurement.count;
    measurement.sum += value;
    measurement.sumSquares += double(value) * value;
  }

  RadioCircuitObservation result(float supplyReferenceVolts) const {
    RadioCircuitObservation result;
    result.supplyReferenceVolts = supplyReferenceVolts;
    for (std::size_t i = 0; i < measurements_.size(); ++i) {
      const Measurement& measurement = measurements_[i];
      if (!measurement.count) continue;
      result.observed |= uint32_t{1} << i;
      const double mean = measurement.sum / measurement.count;
      const double power = measurement.sumSquares / measurement.count;
      result.readings[i].mean = static_cast<float>(mean);
      result.readings[i].rms =
          static_cast<float>(std::sqrt(std::max(0.0, power)));
      result.readings[i].acRms =
          static_cast<float>(std::sqrt(std::max(0.0, power - mean * mean)));
    }
    return result;
  }

 private:
  struct Measurement {
    uint32_t count = 0;
    double sum = 0;
    double sumSquares = 0;
  };
  std::array<Measurement, kRadioCircuitProbeCount> measurements_{};
};
