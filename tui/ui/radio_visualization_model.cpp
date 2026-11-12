#include "radio_visualization_model.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace tui_radio_visualization {

Levels project(const RadioPlaybackObservation& observation) {
  Levels out;
  out.mode = observation.mode;
  out.generation = observation.generation;
  out.observed = observation.circuit.observed;
  constexpr std::array<float, kRadioCircuitProbeCount> reference = {
      0.075f, 0.0000055f, 4.3f, 2.0f, 3.0f, 0.8f, 0.25f, 1.0f};
  for (std::size_t i = 0; i < reference.size(); ++i) {
    const auto probe = static_cast<RadioCircuitProbe>(i);
    if (!observation.circuit.has(probe)) continue;
    const auto& reading = observation.circuit.readings[i];
    if (i <= static_cast<std::size_t>(RadioCircuitProbe::IfEnvelopeVolts)) {
      const float carrier = std::max(0.0f, reading.mean) / reference[i];
      const float modulation = reading.acRms / reference[i];
      out.values[i] = std::clamp(.4f * std::sqrt(carrier) + 1.4f * modulation,
                                 0.0f, 1.0f);
      continue;
    }
    const float amplitude =
        probe == RadioCircuitProbe::DetectorVolts ||
                probe == RadioCircuitProbe::AudioPlateSwingVolts
            ? reading.acRms : reading.rms;
    float scale = reference[i];
    if (probe == RadioCircuitProbe::SupplyVolts) {
      scale = observation.circuit.supplyReferenceVolts;
      if (!(scale > 0.0f)) {
        out.observed &= ~radioCircuitProbeBit(probe);
        continue;
      }
    }
    out.values[i] = std::clamp(std::sqrt(std::max(0.0f, amplitude / scale)),
                               0.0f, 1.0f);
  }
  return out;
}

Presentation present(const Levels* levels, RadioFilterMode requestedMode,
                     bool paused) {
  if (levels) {
    const bool showCircuit = levels->mode == RadioFilterMode::Typical1930s &&
                             levels->observed != 0;
    if (levels->mode != requestedMode) return {Status::Switching, showCircuit};
    if (levels->mode == RadioFilterMode::Off) return {Status::SelectTypical, false};
    if (levels->mode == RadioFilterMode::Philco37116)
      return {Status::UnsupportedReceiver, false};
    if (!showCircuit) return {Status::NoCircuitObservation, false};
    return {paused ? Status::Paused : Status::Playing, true};
  }
  if (requestedMode == RadioFilterMode::Off) return {Status::SelectTypical, false};
  if (requestedMode == RadioFilterMode::Philco37116)
    return {Status::UnsupportedReceiver, false};
  return {paused ? Status::ResumeToObserve : Status::Waiting, false};
}

}  // namespace tui_radio_visualization
