#include "radio_settings_schema.h"

#include <array>
#include <cmath>
#include <type_traits>

#include "../../radio.h"
#include "radio_settings_document.h"

namespace radio_settings {
namespace {

template <typename Accessor>
constexpr Field setting(std::string_view name, Accessor accessor) {
  using T = std::remove_reference_t<std::invoke_result_t<Accessor, Radio1938&>>;
  static_assert(
      std::is_same_v<T, float> || std::is_same_v<T, int> || std::is_same_v<T, bool>,
      "Unsupported radio setting type");
  constexpr auto type = std::is_same_v<T, float> ? ValueType::Number
                        : std::is_same_v<T, int> ? ValueType::Integer
                                                 : ValueType::Boolean;
  return {name, type, Target{+accessor}};
}

constexpr Field node(std::string_view name, PassId id) {
  return {name, ValueType::Boolean, Target{id}};
}

template <size_t N>
constexpr Section section(std::string_view name, const std::array<Field, N>& fields,
                          bool (*validator)(const Radio1938&, std::string*) = nullptr) {
  return {name, fields.data(), N, validator};
}

bool validateRoom(const Radio1938& radio, std::string* error) {
  return validateRadioRoomConfig(radio.room, error);
}

bool validateOversampling(const Radio1938& radio, std::string* error) {
  if (Oversampler::validConfiguration(radio.globals.oversampleFactor,
                                      radio.globals.oversampleCutoffFraction))
    return true;
  if (error)
    *error = "Oversampling requires factor 1, 2, 4 or 8 and cutoff fraction .05..49.";
  return false;
}

bool validateIfStrip(const Radio1938& radio, std::string* error) {
  const auto& s = radio.ifStrip;
  if (!s.enabled) return true;
  bool valid = s.transformerCount >= 1 && s.transformerCount <= 3;
  for (float v : {s.ifCenterHz, s.primaryInductanceHenries, s.secondaryInductanceHenries,
                  s.primarySeriesResistanceOhms, s.secondarySeriesResistanceOhms,
                  s.secondaryLoadResistanceOhms, s.referenceTransimpedanceOhms})
    valid = valid && std::isfinite(v) && v > 0;
  for (float k : {s.interstageCouplingCoeff, s.outputCouplingCoeff})
    valid = valid && std::isfinite(k) && k >= 0 && k < .5f;
  if (!valid && error)
    *error =
        "IF requires 1..3 transformers, positive L/R/reference values and coupling in "
        "[0, .5).";
  return valid;
}

bool validateThermionic(const Radio1938& radio, std::string* error) {
  const auto& d = radio.demod.am;
  const auto& p = radio.powerSupply.config;
  bool valid = true;
  for (const auto* diode : {&d.audioDiode, &d.avcDiode, &p.rectifier})
    valid = valid && diode->perveance > 0 && diode->thermalVolts > 0;
  for (float v : {d.detectorStorageCapFarads, d.audioChargeResistanceOhms,
                  d.avcChargeResistanceOhms, d.avcDischargeResistanceOhms,
                  d.avcFilterCapFarads, p.secondaryResistanceOhms, p.reservoirFarads,
                  p.filterFarads, p.filterResistanceOhms, p.returnResistanceOhms})
    valid = valid && v > 0;
  for (float v :
       {d.audioDischargeResistanceOhms, d.filterResistanceOhms, d.filterCapFarads,
        p.filterHenries, p.returnHenries, p.auxiliaryHenries, p.returnFarads,
        radio.powerSupply.auxiliaryOtherLoadAmps, p.auxiliaryResistanceOhms,
        p.auxiliaryFarads, p.audioFarads, radio.powerSupply.otherLoadAmps,
        radio.powerSupply.screenCurrentRatio, radio.noiseConfig.humHzDefault})
    valid = valid && v >= 0;
  if (!valid && error)
    *error =
        "Thermionic detector/supply requires positive perveance, temperature, "
        "resistance and reservoir capacitors, and nonnegative other L/C/load/mains "
        "values.";
  return valid;
}

bool validateTuning(const Radio1938& radio, std::string* error) {
  const auto& t = radio.tuning;
  const bool valid = t.afcSignalReferenceVolts > 0 && t.afcLoopGain > 0 &&
                     t.smoothTau > 0 && t.updateEps >= 0 && t.afcCaptureHz >= 0 &&
                     t.afcMaxCorrectionHz >= 0 && t.afcResponseMs >= 0 &&
                     t.afcDeadband >= 0 && t.afcDeadband < 1;
  if (!valid && error)
    *error =
        "Tuning requires positive signal reference/smoothing, nonnegative "
        "capture/correction/response and a deadband in [0, 1).";
  return valid;
}

bool validateReceiverInput(const Radio1938& radio, std::string* error) {
  const auto& r = radio.receiverCircuit;
  if (!r.enabled) return true;
  if (!(r.volumeControlResistanceOhms > 0) || !(r.gridLeakResistanceOhms > 0) ||
      !(r.tubePlateSupplyVolts > 0) || !(r.tubeLoadResistanceOhms > 0) ||
      !(r.tubeMu > 0) || r.tubeDcFeedResistanceOhms < 0 || r.couplingCapFarads < 0 ||
      r.volumeControlLoudnessResistanceOhms < 0 || r.volumeControlLoudnessCapFarads < 0 ||
      r.volumeControlPosition < 0 || r.volumeControlPosition > 1 ||
      r.volumeControlTapResistanceOhms < 0 ||
      r.volumeControlTapResistanceOhms > r.volumeControlResistanceOhms) {
    if (error)
      *error =
          "Invalid receiver circuit: positive supply/mu/plate/pot/grid resistance, "
          "nonnegative DC feed resistance, "
          "nonnegative capacitors/loudness resistance, volume in 0..1 and "
          "tap within the pot resistance are required.";
    return false;
  }
  return true;
}

bool validateSpeaker(const Radio1938& radio, std::string* error) {
  const auto& s = radio.speakerStage.speaker;
  const auto optionalRange = [](float v, float minimum, float maximum) {
    return std::isfinite(v) && (v == 0 || (v >= minimum && v <= maximum));
  };
  if (!optionalRange(s.motorTravelMeters, .0001f, .05f) ||
      !optionalRange(s.suspensionTravelMeters, .0001f, .05f) ||
      !optionalRange(s.thermalCapacityJoulesPerKelvin, .01f, 100) ||
      !optionalRange(s.thermalResistanceKelvinPerWatt, .1f, 1000)) {
    if (error)
      *error =
          "Invalid speaker nonlinearity: travel 0 or .0001..05 m, "
          "thermal capacity 0 or .01..100 J/K, resistance 0 or .1..1000 K/W.";
    return false;
  }
  return true;
}

constexpr std::array kGlobalsSettings{
    setting("ifNoiseMix",
            [](Radio1938& radio) -> auto& { return radio.globals.ifNoiseMix; }),
    setting("inputPad", [](Radio1938& radio) -> auto& { return radio.globals.inputPad; }),
    setting("enableAutoLevel",
            [](Radio1938& radio) -> auto& { return radio.globals.enableAutoLevel; }),
    setting("autoTargetDb",
            [](Radio1938& radio) -> auto& { return radio.globals.autoTargetDb; }),
    setting("autoMaxBoostDb",
            [](Radio1938& radio) -> auto& { return radio.globals.autoMaxBoostDb; }),
    setting("outputClipThreshold",
            [](Radio1938& radio) -> auto& { return radio.globals.outputClipThreshold; }),
    setting("oversampleFactor",
            [](Radio1938& radio) -> auto& { return radio.globals.oversampleFactor; }),
    setting(
        "oversampleCutoffFraction",
        [](Radio1938& radio) -> auto& { return radio.globals.oversampleCutoffFraction; }),
    setting("postNoiseMix",
            [](Radio1938& radio) -> auto& { return radio.globals.postNoiseMix; }),
    setting("noiseFloorAmp",
            [](Radio1938& radio) -> auto& { return radio.globals.noiseFloorAmp; }),
};

constexpr std::array kTuningSettings{
    setting("afcLoopGain", [](Radio1938& r) -> auto& { return r.tuning.afcLoopGain; }),
    setting("afcSignalReferenceVolts",
            [](Radio1938& r) -> auto& { return r.tuning.afcSignalReferenceVolts; }),
    setting("tuneOffsetHz",
            [](Radio1938& radio) -> auto& { return radio.tuning.tuneOffsetHz; }),
    setting("safeBwMinHz",
            [](Radio1938& radio) -> auto& { return radio.tuning.safeBwMinHz; }),
    setting("safeBwMaxHz",
            [](Radio1938& radio) -> auto& { return radio.tuning.safeBwMaxHz; }),
    setting("preBwScale",
            [](Radio1938& radio) -> auto& { return radio.tuning.preBwScale; }),
    setting("postBwScale",
            [](Radio1938& radio) -> auto& { return radio.tuning.postBwScale; }),
    setting("smoothTau",
            [](Radio1938& radio) -> auto& { return radio.tuning.smoothTau; }),
    setting("updateEps",
            [](Radio1938& radio) -> auto& { return radio.tuning.updateEps; }),
    setting("magneticTuningEnabled",
            [](Radio1938& radio) -> auto& { return radio.tuning.magneticTuningEnabled; }),
    setting("afcCaptureHz",
            [](Radio1938& radio) -> auto& { return radio.tuning.afcCaptureHz; }),
    setting("afcMaxCorrectionHz",
            [](Radio1938& radio) -> auto& { return radio.tuning.afcMaxCorrectionHz; }),
    setting("afcDeadband",
            [](Radio1938& radio) -> auto& { return radio.tuning.afcDeadband; }),
    setting("afcResponseMs",
            [](Radio1938& radio) -> auto& { return radio.tuning.afcResponseMs; }),
};

constexpr std::array kFrontEndSettings{
    setting("inputHpHz",
            [](Radio1938& radio) -> auto& { return radio.frontEnd.inputHpHz; }),
    setting("rfGain", [](Radio1938& radio) -> auto& { return radio.frontEnd.rfGain; }),
    setting("avcGainDepth",
            [](Radio1938& radio) -> auto& { return radio.frontEnd.avcGainDepth; }),
    setting("selectivityPeakHz",
            [](Radio1938& radio) -> auto& { return radio.frontEnd.selectivityPeakHz; }),
    setting("selectivityPeakQ",
            [](Radio1938& radio) -> auto& { return radio.frontEnd.selectivityPeakQ; }),
    setting(
        "selectivityPeakGainDb",
        [](Radio1938& radio) -> auto& { return radio.frontEnd.selectivityPeakGainDb; }),
    setting("antennaInductanceHenries",
            [](Radio1938& radio) -> auto& {
              return radio.frontEnd.antennaInductanceHenries;
            }),
    setting("antennaCapacitanceFarads",
            [](Radio1938& radio) -> auto& {
              return radio.frontEnd.antennaCapacitanceFarads;
            }),
    setting("antennaSeriesResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.frontEnd.antennaSeriesResistanceOhms;
            }),
    setting("antennaLoadResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.frontEnd.antennaLoadResistanceOhms;
            }),
    setting("rfInductanceHenries",
            [](Radio1938& radio) -> auto& { return radio.frontEnd.rfInductanceHenries; }),
    setting("rfCapacitanceFarads",
            [](Radio1938& radio) -> auto& { return radio.frontEnd.rfCapacitanceFarads; }),
    setting(
        "rfSeriesResistanceOhms",
        [](Radio1938& radio) -> auto& { return radio.frontEnd.rfSeriesResistanceOhms; }),
    setting(
        "rfLoadResistanceOhms",
        [](Radio1938& radio) -> auto& { return radio.frontEnd.rfLoadResistanceOhms; }),
};

constexpr std::array kMixerSettings{
    setting("rfGridDriveVolts",
            [](Radio1938& radio) -> auto& { return radio.mixer.rfGridDriveVolts; }),
    setting("loGridDriveVolts",
            [](Radio1938& radio) -> auto& { return radio.mixer.loGridDriveVolts; }),
    setting("loGridBiasVolts",
            [](Radio1938& radio) -> auto& { return radio.mixer.loGridBiasVolts; }),
    setting("avcGridDriveVolts",
            [](Radio1938& radio) -> auto& { return radio.mixer.avcGridDriveVolts; }),
    setting("plateSupplyVolts",
            [](Radio1938& radio) -> auto& { return radio.mixer.plateSupplyVolts; }),
    setting("plateDcVolts",
            [](Radio1938& radio) -> auto& { return radio.mixer.plateDcVolts; }),
    setting("plateQuiescentVolts",
            [](Radio1938& radio) -> auto& { return radio.mixer.plateQuiescentVolts; }),
    setting("screenVolts",
            [](Radio1938& radio) -> auto& { return radio.mixer.screenVolts; }),
    setting("biasVolts", [](Radio1938& radio) -> auto& { return radio.mixer.biasVolts; }),
    setting("cutoffVolts",
            [](Radio1938& radio) -> auto& { return radio.mixer.cutoffVolts; }),
    setting("modelCutoffVolts",
            [](Radio1938& radio) -> auto& { return radio.mixer.modelCutoffVolts; }),
    setting("plateCurrentAmps",
            [](Radio1938& radio) -> auto& { return radio.mixer.plateCurrentAmps; }),
    setting(
        "plateQuiescentCurrentAmps",
        [](Radio1938& radio) -> auto& { return radio.mixer.plateQuiescentCurrentAmps; }),
    setting(
        "mutualConductanceSiemens",
        [](Radio1938& radio) -> auto& { return radio.mixer.mutualConductanceSiemens; }),
    setting("acLoadResistanceOhms",
            [](Radio1938& radio) -> auto& { return radio.mixer.acLoadResistanceOhms; }),
    setting("plateKneeVolts",
            [](Radio1938& radio) -> auto& { return radio.mixer.plateKneeVolts; }),
    setting("gridSoftnessVolts",
            [](Radio1938& radio) -> auto& { return radio.mixer.gridSoftnessVolts; }),
    setting("plateResistanceOhms",
            [](Radio1938& radio) -> auto& { return radio.mixer.plateResistanceOhms; }),
    setting("operatingPointToleranceVolts",
            [](Radio1938& radio) -> auto& {
              return radio.mixer.operatingPointToleranceVolts;
            }),
};

constexpr std::array kIfStripSettings{
    setting("primarySeriesResistanceOhms",
            [](Radio1938& r) -> auto& { return r.ifStrip.primarySeriesResistanceOhms; }),
    setting(
        "secondarySeriesResistanceOhms",
        [](Radio1938& r) -> auto& { return r.ifStrip.secondarySeriesResistanceOhms; }),
    setting("referenceTransimpedanceOhms",
            [](Radio1938& r) -> auto& { return r.ifStrip.referenceTransimpedanceOhms; }),
    setting("transformerCount",
            [](Radio1938& r) -> auto& { return r.ifStrip.transformerCount; }),
    setting("enabled", [](Radio1938& radio) -> auto& { return radio.ifStrip.enabled; }),
    setting("ifMinBwHz",
            [](Radio1938& radio) -> auto& { return radio.ifStrip.ifMinBwHz; }),
    setting("stageGain",
            [](Radio1938& radio) -> auto& { return radio.ifStrip.stageGain; }),
    setting("avcGainDepth",
            [](Radio1938& radio) -> auto& { return radio.ifStrip.avcGainDepth; }),
    setting("ifCenterHz",
            [](Radio1938& radio) -> auto& { return radio.ifStrip.ifCenterHz; }),
    setting(
        "primaryInductanceHenries",
        [](Radio1938& radio) -> auto& { return radio.ifStrip.primaryInductanceHenries; }),
    setting("secondaryInductanceHenries",
            [](Radio1938& radio) -> auto& {
              return radio.ifStrip.secondaryInductanceHenries;
            }),
    setting("secondaryLoadResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.ifStrip.secondaryLoadResistanceOhms;
            }),
    setting(
        "interstageCouplingCoeff",
        [](Radio1938& radio) -> auto& { return radio.ifStrip.interstageCouplingCoeff; }),
    setting("outputCouplingCoeff",
            [](Radio1938& radio) -> auto& { return radio.ifStrip.outputCouplingCoeff; }),
};

constexpr std::array kDemodSettings{
    setting(
        "filterResistanceOhms",
        [](Radio1938& radio) -> auto& { return radio.demod.am.filterResistanceOhms; }),
    setting("filterCapFarads",
            [](Radio1938& radio) -> auto& { return radio.demod.am.filterCapFarads; }),
    setting(
        "audioDiodePerveance",
        [](Radio1938& radio) -> auto& { return radio.demod.am.audioDiode.perveance; }),
    setting(
        "audioDiodeThermalVolts",
        [](Radio1938& radio) -> auto& { return radio.demod.am.audioDiode.thermalVolts; }),
    setting("audioDiodeContactPotentialVolts",
            [](Radio1938& radio) -> auto& {
              return radio.demod.am.audioDiode.contactPotentialVolts;
            }),
    setting("avcDiodePerveance",
            [](Radio1938& radio) -> auto& { return radio.demod.am.avcDiode.perveance; }),
    setting(
        "avcDiodeThermalVolts",
        [](Radio1938& radio) -> auto& { return radio.demod.am.avcDiode.thermalVolts; }),
    setting("avcDiodeContactPotentialVolts",
            [](Radio1938& radio) -> auto& {
              return radio.demod.am.avcDiode.contactPotentialVolts;
            }),
    setting("avcDelayBiasVolts",
            [](Radio1938& radio) -> auto& { return radio.demod.am.avcDelayBiasVolts; }),
    setting("detectorStorageCapFarads",
            [](Radio1938& radio) -> auto& {
              return radio.demod.am.detectorStorageCapFarads;
            }),
    setting("audioChargeResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.demod.am.audioChargeResistanceOhms;
            }),
    setting("audioDischargeResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.demod.am.audioDischargeResistanceOhms;
            }),
    setting(
        "avcChargeResistanceOhms",
        [](Radio1938& radio) -> auto& { return radio.demod.am.avcChargeResistanceOhms; }),
    setting("avcDischargeResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.demod.am.avcDischargeResistanceOhms;
            }),
    setting("avcFilterCapFarads",
            [](Radio1938& radio) -> auto& { return radio.demod.am.avcFilterCapFarads; }),
    setting("controlVoltageRef",
            [](Radio1938& radio) -> auto& { return radio.demod.am.controlVoltageRef; }),
    setting("senseLowHz",
            [](Radio1938& radio) -> auto& { return radio.demod.am.senseLowHz; }),
    setting("senseHighHz",
            [](Radio1938& radio) -> auto& { return radio.demod.am.senseHighHz; }),
    setting("afcSenseLpHz",
            [](Radio1938& radio) -> auto& { return radio.demod.am.afcSenseLpHz; }),
    setting("afcsoffsethz",
            [](Radio1938& radio) -> auto& { return radio.demod.am.afcLowOffsetHz; }),
    setting("afchoffsethz",
            [](Radio1938& radio) -> auto& { return radio.demod.am.afcHighOffsetHz; }),
    setting("afcLowStep",
            [](Radio1938& radio) -> auto& { return radio.demod.am.afcLowStep; }),
    setting("afcHighStep",
            [](Radio1938& radio) -> auto& { return radio.demod.am.afcHighStep; }),
    setting("afcLowPhase",
            [](Radio1938& radio) -> auto& { return radio.demod.am.afcLowPhase; }),
    setting("afcHighPhase",
            [](Radio1938& radio) -> auto& { return radio.demod.am.afcHighPhase; }),
};

constexpr std::array kReceiverCircuitSettings{
    setting(
        "tubeDcFeedResistanceOhms",
        [](Radio1938& r) -> auto& { return r.receiverCircuit.tubeDcFeedResistanceOhms; }),
    setting("enabled",
            [](Radio1938& radio) -> auto& { return radio.receiverCircuit.enabled; }),
    setting("volumeControlResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.receiverCircuit.volumeControlResistanceOhms;
            }),
    setting("volumeControlTapResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.receiverCircuit.volumeControlTapResistanceOhms;
            }),
    setting("volumeControlPosition",
            [](Radio1938& radio) -> auto& {
              return radio.receiverCircuit.volumeControlPosition;
            }),
    setting("volumeControlLoudnessResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.receiverCircuit.volumeControlLoudnessResistanceOhms;
            }),
    setting("volumeControlLoudnessCapFarads",
            [](Radio1938& radio) -> auto& {
              return radio.receiverCircuit.volumeControlLoudnessCapFarads;
            }),
    setting("volumeControlTapVoltage",
            [](Radio1938& radio) -> auto& {
              return radio.receiverCircuit.volumeControlTapVoltage;
            }),
    setting("couplingCapFarads",
            [](Radio1938& radio) -> auto& {
              return radio.receiverCircuit.couplingCapFarads;
            }),
    setting("gridLeakResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.receiverCircuit.gridLeakResistanceOhms;
            }),
    setting("tubePlateSupplyVolts",
            [](Radio1938& radio) -> auto& {
              return radio.receiverCircuit.tubePlateSupplyVolts;
            }),
    setting(
        "tubePlateDcVolts",
        [](Radio1938& radio) -> auto& { return radio.receiverCircuit.tubePlateDcVolts; }),
    setting("tubeQuiescentPlateVolts",
            [](Radio1938& radio) -> auto& {
              return radio.receiverCircuit.tubeQuiescentPlateVolts;
            }),
    setting(
        "tubeBiasVolts",
        [](Radio1938& radio) -> auto& { return radio.receiverCircuit.tubeBiasVolts; }),
    setting("tubeQuiescentPlateCurrentAmps",
            [](Radio1938& radio) -> auto& {
              return radio.receiverCircuit.tubeQuiescentPlateCurrentAmps;
            }),
    setting("tubeMu",
            [](Radio1938& radio) -> auto& { return radio.receiverCircuit.tubeMu; }),
    setting("tubeTriodeConnected",
            [](Radio1938& radio) -> auto& {
              return radio.receiverCircuit.tubeTriodeConnected;
            }),
    setting("tubeLoadResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.receiverCircuit.tubeLoadResistanceOhms;
            }),
    setting("tubeplatresistanceohms",
            [](Radio1938& radio) -> auto& {
              return radio.receiverCircuit.tubePlateResistanceOhms;
            }),
    setting("operatingPointToleranceVolts",
            [](Radio1938& radio) -> auto& {
              return radio.receiverCircuit.operatingPointToleranceVolts;
            }),
    setting(
        "tubePlateVoltage",
        [](Radio1938& radio) -> auto& { return radio.receiverCircuit.tubePlateVoltage; }),
};

constexpr std::array kToneSettings{
    setting("presenceHz",
            [](Radio1938& radio) -> auto& { return radio.tone.presenceHz; }),
    setting("presenceQ", [](Radio1938& radio) -> auto& { return radio.tone.presenceQ; }),
    setting("presenceGainDb",
            [](Radio1938& radio) -> auto& { return radio.tone.presenceGainDb; }),
    setting("tiltSplitHz",
            [](Radio1938& radio) -> auto& { return radio.tone.tiltSplitHz; }),
    setting("tiltDepthDb",
            [](Radio1938& radio) -> auto& { return radio.tone.tiltDepthDb; }),
};

constexpr std::array kPowerSupplySettings{
    setting("auxiliaryHenries",
            [](Radio1938& radio) -> auto& {
              return radio.powerSupply.config.auxiliaryHenries;
            }),
    setting("auxiliaryOtherLoadAmps",
            [](Radio1938& radio) -> auto& {
              return radio.powerSupply.auxiliaryOtherLoadAmps;
            }),
    setting("secondaryResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.powerSupply.config.secondaryResistanceOhms;
            }),
    setting("reservoirFarads",
            [](Radio1938& radio) -> auto& {
              return radio.powerSupply.config.reservoirFarads;
            }),
    setting(
        "filterFarads",
        [](Radio1938& radio) -> auto& { return radio.powerSupply.config.filterFarads; }),
    setting("filterReturnsToCentreTap",
            [](Radio1938& radio) -> auto& {
              return radio.powerSupply.config.filterReturnsToCentreTap;
            }),
    setting(
        "filterHenries",
        [](Radio1938& radio) -> auto& { return radio.powerSupply.config.filterHenries; }),
    setting("filterResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.powerSupply.config.filterResistanceOhms;
            }),
    setting(
        "returnFarads",
        [](Radio1938& radio) -> auto& { return radio.powerSupply.config.returnFarads; }),
    setting(
        "returnHenries",
        [](Radio1938& radio) -> auto& { return radio.powerSupply.config.returnHenries; }),
    setting("returnResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.powerSupply.config.returnResistanceOhms;
            }),
    setting("auxiliaryResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.powerSupply.config.auxiliaryResistanceOhms;
            }),
    setting("auxiliaryFarads",
            [](Radio1938& radio) -> auto& {
              return radio.powerSupply.config.auxiliaryFarads;
            }),
    setting(
        "audioFarads",
        [](Radio1938& radio) -> auto& { return radio.powerSupply.config.audioFarads; }),
    setting("rectifierPerveance",
            [](Radio1938& radio) -> auto& {
              return radio.powerSupply.config.rectifier.perveance;
            }),
    setting("rectifierThermalVolts",
            [](Radio1938& radio) -> auto& {
              return radio.powerSupply.config.rectifier.thermalVolts;
            }),
    setting("rectifierContactPotentialVolts",
            [](Radio1938& radio) -> auto& {
              return radio.powerSupply.config.rectifier.contactPotentialVolts;
            }),
    setting("otherLoadAmps",
            [](Radio1938& radio) -> auto& { return radio.powerSupply.otherLoadAmps; }),
    setting(
        "screenCurrentRatio",
        [](Radio1938& radio) -> auto& { return radio.powerSupply.screenCurrentRatio; }),
};

constexpr std::array kPowerSettings{
    setting("postLpHz", [](Radio1938& radio) -> auto& { return radio.power.postLpHz; }),
    setting("gridCouplingCapFarads",
            [](Radio1938& radio) -> auto& { return radio.power.gridCouplingCapFarads; }),
    setting("gridLeakResistanceOhms",
            [](Radio1938& radio) -> auto& { return radio.power.gridLeakResistanceOhms; }),
    setting(
        "driverSourceResistanceOhms",
        [](Radio1938& radio) -> auto& { return radio.power.driverSourceResistanceOhms; }),
    setting("tubePlateSupplyVolts",
            [](Radio1938& radio) -> auto& { return radio.power.tubePlateSupplyVolts; }),
    setting("tubePlateDcVolts",
            [](Radio1938& radio) -> auto& { return radio.power.tubePlateDcVolts; }),
    setting(
        "tubeQuiescentPlateVolts",
        [](Radio1938& radio) -> auto& { return radio.power.tubeQuiescentPlateVolts; }),
    setting("tubeScreenVolts",
            [](Radio1938& radio) -> auto& { return radio.power.tubeScreenVolts; }),
    setting("tubeBiasVolts",
            [](Radio1938& radio) -> auto& { return radio.power.tubeBiasVolts; }),
    setting("tubePlateCurrentAmps",
            [](Radio1938& radio) -> auto& { return radio.power.tubePlateCurrentAmps; }),
    setting("tubeQuiescentPlateCurrentAmps",
            [](Radio1938& radio) -> auto& {
              return radio.power.tubeQuiescentPlateCurrentAmps;
            }),
    setting("tubeMutualConductanceSiemens",
            [](Radio1938& radio) -> auto& {
              return radio.power.tubeMutualConductanceSiemens;
            }),
    setting("tubeMu", [](Radio1938& radio) -> auto& { return radio.power.tubeMu; }),
    setting("tubeTriodeConnected",
            [](Radio1938& radio) -> auto& { return radio.power.tubeTriodeConnected; }),
    setting(
        "tubeAcLoadResistanceOhms",
        [](Radio1938& radio) -> auto& { return radio.power.tubeAcLoadResistanceOhms; }),
    setting("tubePlateKneeVolts",
            [](Radio1938& radio) -> auto& { return radio.power.tubePlateKneeVolts; }),
    setting("tubeGridSoftnessVolts",
            [](Radio1938& radio) -> auto& { return radio.power.tubeGridSoftnessVolts; }),
    setting("tubeGridCurrentResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.power.tubeGridCurrentResistanceOhms;
            }),
    setting(
        "tubeplatresistanceohms",
        [](Radio1938& radio) -> auto& { return radio.power.tubePlateResistanceOhms; }),
    setting("operatingPointToleranceVolts",
            [](Radio1938& radio) -> auto& {
              return radio.power.operatingPointToleranceVolts;
            }),
    setting("tubePlateVoltage",
            [](Radio1938& radio) -> auto& { return radio.power.tubePlateVoltage; }),
    setting("interstagePrimaryLeakageInductanceHenries",
            [](Radio1938& radio) -> auto& {
              return radio.power.interstagePrimaryLeakageInductanceHenries;
            }),
    setting("interstageMagnetizingInductanceHenries",
            [](Radio1938& radio) -> auto& {
              return radio.power.interstageMagnetizingInductanceHenries;
            }),
    setting("interstagePrimaryResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.power.interstagePrimaryResistanceOhms;
            }),
    setting("interstagePrimaryCoreLossResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.power.interstagePrimaryCoreLossResistanceOhms;
            }),
    setting("interstageTurnsRatioPrimaryToSecondary",
            [](Radio1938& radio) -> auto& {
              return radio.power.interstageTurnsRatioPrimaryToSecondary;
            }),
    setting("interstagePrimaryShuntCapFarads",
            [](Radio1938& radio) -> auto& {
              return radio.power.interstagePrimaryShuntCapFarads;
            }),
    setting("interstageSecondaryLeakageInductanceHenries",
            [](Radio1938& radio) -> auto& {
              return radio.power.interstageSecondaryLeakageInductanceHenries;
            }),
    setting("interstageSecondaryResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.power.interstageSecondaryResistanceOhms;
            }),
    setting("interstageSecondaryShuntCapFarads",
            [](Radio1938& radio) -> auto& {
              return radio.power.interstageSecondaryShuntCapFarads;
            }),
    setting("interstageIntegrationSubsteps",
            [](Radio1938& radio) -> auto& {
              return radio.power.interstageIntegrationSubsteps;
            }),
    setting("outputGridLeakResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputGridLeakResistanceOhms;
            }),
    setting("outputGridCurrentResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputGridCurrentResistanceOhms;
            }),
    setting(
        "outputTubePlateSupplyVolts",
        [](Radio1938& radio) -> auto& { return radio.power.outputTubePlateSupplyVolts; }),
    setting("outputTubePlateDcVolts",
            [](Radio1938& radio) -> auto& { return radio.power.outputTubePlateDcVolts; }),
    setting("outputTubeQuiescentPlateVolts",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputTubeQuiescentPlateVolts;
            }),
    setting("outputTubeBiasVolts",
            [](Radio1938& radio) -> auto& { return radio.power.outputTubeBiasVolts; }),
    setting(
        "outputTubePlateCurrentAmps",
        [](Radio1938& radio) -> auto& { return radio.power.outputTubePlateCurrentAmps; }),
    setting("outputTubeQuiescentPlateCurrentAmps",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputTubeQuiescentPlateCurrentAmps;
            }),
    setting("outputTubeMutualConductanceSiemens",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputTubeMutualConductanceSiemens;
            }),
    setting("outputTubeMu",
            [](Radio1938& radio) -> auto& { return radio.power.outputTubeMu; }),
    setting("outputTubePlateToPlateLoadOhms",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputTubePlateToPlateLoadOhms;
            }),
    setting(
        "outputTubePlateKneeVolts",
        [](Radio1938& radio) -> auto& { return radio.power.outputTubePlateKneeVolts; }),
    setting("outputTubeGridSoftnessVolts",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputTubeGridSoftnessVolts;
            }),
    setting("outputtubeplatresistanceohms",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputTubePlateResistanceOhms;
            }),
    setting("outputGridAVolts",
            [](Radio1938& radio) -> auto& { return radio.power.outputGridAVolts; }),
    setting("outputGridBVolts",
            [](Radio1938& radio) -> auto& { return radio.power.outputGridBVolts; }),
    setting("outputOperatingPointToleranceVolts",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputOperatingPointToleranceVolts;
            }),
    setting("outputTransformerPrimaryLeakageInductanceHenries",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputTransformerPrimaryLeakageInductanceHenries;
            }),
    setting("outputTransformerMagnetizingInductanceHenries",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputTransformerMagnetizingInductanceHenries;
            }),
    setting("outputTransformerTurnsRatioPrimaryToSecondary",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputTransformerTurnsRatioPrimaryToSecondary;
            }),
    setting("outputTransformerPrimaryResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputTransformerPrimaryResistanceOhms;
            }),
    setting("outputTransformerPrimaryCoreLossResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputTransformerPrimaryCoreLossResistanceOhms;
            }),
    setting("outputTransformerPrimaryShuntCapFarads",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputTransformerPrimaryShuntCapFarads;
            }),
    setting("outputTransformerSecondaryLeakageInductanceHenries",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputTransformerSecondaryLeakageInductanceHenries;
            }),
    setting("outputTransformerSecondaryResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputTransformerSecondaryResistanceOhms;
            }),
    setting("outputTransformerSecondaryShuntCapFarads",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputTransformerSecondaryShuntCapFarads;
            }),
    setting("outputTransformerIntegrationSubsteps",
            [](Radio1938& radio) -> auto& {
              return radio.power.outputTransformerIntegrationSubsteps;
            }),
    setting(
        "outputLoadResistanceOhms",
        [](Radio1938& radio) -> auto& { return radio.power.outputLoadResistanceOhms; }),
    setting(
        "nominalOutputPowerWatts",
        [](Radio1938& radio) -> auto& { return radio.power.nominalOutputPowerWatts; }),
};

constexpr std::array kNoiseSettings{
    setting("humMainsPickupMix",
            [](Radio1938& r) -> auto& { return r.noiseRuntime.hum.humMainsPickupMix; }),
    setting("humThirdHarmonicMix",
            [](Radio1938& r) -> auto& { return r.noiseRuntime.hum.humThirdHarmonicMix; }),
    setting("enableHumTone",
            [](Radio1938& radio) -> auto& { return radio.noiseConfig.enableHumTone; }),
    setting("humHzDefault",
            [](Radio1938& radio) -> auto& { return radio.noiseConfig.humHzDefault; }),
    setting("noiseref",
            [](Radio1938& radio) -> auto& { return radio.noiseConfig.noiseWeightRef; }),
    setting(
        "noiseWeightScaleMax",
        [](Radio1938& radio) -> auto& { return radio.noiseConfig.noiseWeightScaleMax; }),
    setting("humAmpScale",
            [](Radio1938& radio) -> auto& { return radio.noiseConfig.humAmpScale; }),
    setting("crackleAmpScale",
            [](Radio1938& radio) -> auto& { return radio.noiseConfig.crackleAmpScale; }),
    setting("crackleRateScale",
            [](Radio1938& radio) -> auto& { return radio.noiseConfig.crackleRateScale; }),
    setting("noiseHpHz",
            [](Radio1938& radio) -> auto& { return radio.noiseRuntime.hum.noiseHpHz; }),
    setting("noiseLpHz",
            [](Radio1938& radio) -> auto& { return radio.noiseRuntime.hum.noiseLpHz; }),
    setting("filterQ",
            [](Radio1938& radio) -> auto& { return radio.noiseRuntime.hum.filterQ; }),
    setting(
        "pinkFastPole",
        [](Radio1938& radio) -> auto& { return radio.noiseRuntime.hum.pinkFastPole; }),
    setting(
        "pinkSlowPole",
        [](Radio1938& radio) -> auto& { return radio.noiseRuntime.hum.pinkSlowPole; }),
    setting("brownStep",
            [](Radio1938& radio) -> auto& { return radio.noiseRuntime.hum.brownStep; }),
    setting(
        "hissDriftPole",
        [](Radio1938& radio) -> auto& { return radio.noiseRuntime.hum.hissDriftPole; }),
    setting(
        "hissDriftNoise",
        [](Radio1938& radio) -> auto& { return radio.noiseRuntime.hum.hissDriftNoise; }),
    setting("hissDriftSlowPole",
            [](Radio1938& radio) -> auto& {
              return radio.noiseRuntime.hum.hissDriftSlowPole;
            }),
    setting("hissDriftSlowNoise",
            [](Radio1938& radio) -> auto& {
              return radio.noiseRuntime.hum.hissDriftSlowNoise;
            }),
    setting("whiteMix",
            [](Radio1938& radio) -> auto& { return radio.noiseRuntime.hum.whiteMix; }),
    setting("pinkFastMix",
            [](Radio1938& radio) -> auto& { return radio.noiseRuntime.hum.pinkFastMix; }),
    setting("pinkDifferenceMix",
            [](Radio1938& radio) -> auto& {
              return radio.noiseRuntime.hum.pinkDifferenceMix;
            }),
    setting("pinkFastSubtract",
            [](Radio1938& radio) -> auto& {
              return radio.noiseRuntime.hum.pinkFastSubtract;
            }),
    setting("brownMix",
            [](Radio1938& radio) -> auto& { return radio.noiseRuntime.hum.brownMix; }),
    setting("hissBase",
            [](Radio1938& radio) -> auto& { return radio.noiseRuntime.hum.hissBase; }),
    setting(
        "hissDriftDepth",
        [](Radio1938& radio) -> auto& { return radio.noiseRuntime.hum.hissDriftDepth; }),
    setting("hissDriftSlowMix",
            [](Radio1938& radio) -> auto& {
              return radio.noiseRuntime.hum.hissDriftSlowMix;
            }),
    setting("humSecondHarmonicMix",
            [](Radio1938& radio) -> auto& {
              return radio.noiseRuntime.hum.humSecondHarmonicMix;
            }),
};

constexpr std::array kSpeakerSettings{
    setting("drive", [](Radio1938& radio) -> auto& { return radio.speakerStage.drive; }),
    setting("suspensionHz",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.suspensionHz;
            }),
    setting(
        "suspensionQ",
        [](Radio1938& radio) -> auto& { return radio.speakerStage.speaker.suspensionQ; }),
    setting("suspensionGainDb",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.suspensionGainDb;
            }),
    setting(
        "coneBodyHz",
        [](Radio1938& radio) -> auto& { return radio.speakerStage.speaker.coneBodyHz; }),
    setting(
        "coneBodyQ",
        [](Radio1938& radio) -> auto& { return radio.speakerStage.speaker.coneBodyQ; }),
    setting("coneBodyGainDb",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.coneBodyGainDb;
            }),
    setting("upperBreakupHz",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.upperBreakupHz;
            }),
    setting("upperBreakupQ",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.upperBreakupQ;
            }),
    setting("upperBreakupGainDb",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.upperBreakupGainDb;
            }),
    setting(
        "coneDipHz",
        [](Radio1938& radio) -> auto& { return radio.speakerStage.speaker.coneDipHz; }),
    setting(
        "coneDipQ",
        [](Radio1938& radio) -> auto& { return radio.speakerStage.speaker.coneDipQ; }),
    setting("coneDipGainDb",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.coneDipGainDb;
            }),
    setting("topLpHz",
            [](Radio1938& radio) -> auto& { return radio.speakerStage.speaker.topLpHz; }),
    setting(
        "hfLossLpHz",
        [](Radio1938& radio) -> auto& { return radio.speakerStage.speaker.hfLossLpHz; }),
    setting(
        "hfLossDepth",
        [](Radio1938& radio) -> auto& { return radio.speakerStage.speaker.hfLossDepth; }),
    setting("filterQ",
            [](Radio1938& radio) -> auto& { return radio.speakerStage.speaker.filterQ; }),
    setting("limit",
            [](Radio1938& radio) -> auto& { return radio.speakerStage.speaker.limit; }),
    setting(
        "asymBias",
        [](Radio1938& radio) -> auto& { return radio.speakerStage.speaker.asymBias; }),
    setting("suspensionComplianceTolerance",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.suspensionComplianceTolerance;
            }),
    setting("coneMassTolerance",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.coneMassTolerance;
            }),
    setting("breakupTolerance",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.breakupTolerance;
            }),
    setting("voiceCoilTolerance",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.voiceCoilTolerance;
            }),
    setting("motorTravelMeters",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.motorTravelMeters;
            }),
    setting("suspensionTravelMeters",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.suspensionTravelMeters;
            }),
    setting("thermalCapacityJoulesPerKelvin",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.thermalCapacityJoulesPerKelvin;
            }),
    setting("thermalResistanceKelvinPerWatt",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.thermalResistanceKelvinPerWatt;
            }),
    setting("voiceCoilResistanceOhms",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.voiceCoilResistanceOhms;
            }),
    setting("voiceCoilInductanceHenries",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.voiceCoilInductanceHenries;
            }),
    setting("movingMassKg",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.movingMassKg;
            }),
    setting(
        "mechanicalQ",
        [](Radio1938& radio) -> auto& { return radio.speakerStage.speaker.mechanicalQ; }),
    setting(
        "electricalQ",
        [](Radio1938& radio) -> auto& { return radio.speakerStage.speaker.electricalQ; }),
    setting("forceFactorBl",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.forceFactorBl;
            }),
    setting("suspensionComplianceMetersPerNewton",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.suspensionComplianceMetersPerNewton;
            }),
    setting("mechanicalDampingNsPerMeter",
            [](Radio1938& radio) -> auto& {
              return radio.speakerStage.speaker.mechanicalDampingNsPerMeter;
            }),
};

constexpr std::array kCabinetSettings{
    setting("enabled", [](Radio1938& radio) -> auto& { return radio.cabinet.enabled; }),
    setting("mountingHpHz",
            [](Radio1938& radio) -> auto& { return radio.cabinet.mountingHpHz; }),
    setting("mountingHpQ",
            [](Radio1938& radio) -> auto& { return radio.cabinet.mountingHpQ; }),
    setting("panelHz", [](Radio1938& radio) -> auto& { return radio.cabinet.panelHz; }),
    setting("panelQ", [](Radio1938& radio) -> auto& { return radio.cabinet.panelQ; }),
    setting("panelGainDb",
            [](Radio1938& radio) -> auto& { return radio.cabinet.panelGainDb; }),
    setting("chassisHz",
            [](Radio1938& radio) -> auto& { return radio.cabinet.chassisHz; }),
    setting("chassisQ", [](Radio1938& radio) -> auto& { return radio.cabinet.chassisQ; }),
    setting("chassisGainDb",
            [](Radio1938& radio) -> auto& { return radio.cabinet.chassisGainDb; }),
    setting("cavityDipHz",
            [](Radio1938& radio) -> auto& { return radio.cabinet.cavityDipHz; }),
    setting("cavityDipQ",
            [](Radio1938& radio) -> auto& { return radio.cabinet.cavityDipQ; }),
    setting("cavityDipGainDb",
            [](Radio1938& radio) -> auto& { return radio.cabinet.cavityDipGainDb; }),
    setting("grilleLpHz",
            [](Radio1938& radio) -> auto& { return radio.cabinet.grilleLpHz; }),
    setting("rearDelayMs",
            [](Radio1938& radio) -> auto& { return radio.cabinet.rearDelayMs; }),
    setting("rearMix", [](Radio1938& radio) -> auto& { return radio.cabinet.rearMix; }),
    setting("rearHpHz", [](Radio1938& radio) -> auto& { return radio.cabinet.rearHpHz; }),
    setting("rearLpHz", [](Radio1938& radio) -> auto& { return radio.cabinet.rearLpHz; }),
};

constexpr std::array kFinalLimiterSettings{
    setting("enabled",
            [](Radio1938& radio) -> auto& { return radio.finalLimiter.enabled; }),
    setting("threshold",
            [](Radio1938& radio) -> auto& { return radio.finalLimiter.threshold; }),
    setting("lookaheadMs",
            [](Radio1938& radio) -> auto& { return radio.finalLimiter.lookaheadMs; }),
    setting("attackMs",
            [](Radio1938& radio) -> auto& { return radio.finalLimiter.attackMs; }),
    setting("releaseMs",
            [](Radio1938& radio) -> auto& { return radio.finalLimiter.releaseMs; }),
};

constexpr std::array kRoomSettings{
    setting("enabled", [](Radio1938& radio) -> auto& { return radio.room.enabled; }),
    setting("headphones",
            [](Radio1938& radio) -> auto& { return radio.room.headphones; }),
    setting("reflections",
            [](Radio1938& radio) -> auto& { return radio.room.reflections; }),
    setting("decaySeconds",
            [](Radio1938& radio) -> auto& { return radio.room.decaySeconds; }),
    setting("lateLevel", [](Radio1938& radio) -> auto& { return radio.room.lateLevel; }),
    setting("widthMeters",
            [](Radio1938& radio) -> auto& { return radio.room.widthMeters; }),
    setting("lengthMeters",
            [](Radio1938& radio) -> auto& { return radio.room.lengthMeters; }),
    setting("heightMeters",
            [](Radio1938& radio) -> auto& { return radio.room.heightMeters; }),
    setting("sourceXMeters",
            [](Radio1938& radio) -> auto& { return radio.room.sourceXMeters; }),
    setting("sourceYMeters",
            [](Radio1938& radio) -> auto& { return radio.room.sourceYMeters; }),
    setting("sourceZMeters",
            [](Radio1938& radio) -> auto& { return radio.room.sourceZMeters; }),
    setting("listenerXMeters",
            [](Radio1938& radio) -> auto& { return radio.room.listenerXMeters; }),
    setting("listenerYMeters",
            [](Radio1938& radio) -> auto& { return radio.room.listenerYMeters; }),
    setting("listenerZMeters",
            [](Radio1938& radio) -> auto& { return radio.room.listenerZMeters; }),
    setting("speakerYawDegrees",
            [](Radio1938& radio) -> auto& { return radio.room.speakerYawDegrees; }),
    setting("headYawDegrees",
            [](Radio1938& radio) -> auto& { return radio.room.headYawDegrees; }),
    setting("wallLowpassHz",
            [](Radio1938& radio) -> auto& { return radio.room.wallLowpassHz; }),
};

constexpr std::array kOutputSettings{
    setting("digitalMakeupGain",
            [](Radio1938& radio) -> auto& { return radio.output.digitalMakeupGain; }),
};

constexpr std::array kNodesSettings{
    node("tuning", PassId::Tuning),
    node("input", PassId::Input),
    node("avc", PassId::AVC),
    node("afc", PassId::AFC),
    node("controlbus", PassId::ControlBus),
    node("interferencederived", PassId::InterferenceDerived),
    node("frontend", PassId::FrontEnd),
    node("mixer", PassId::Mixer),
    node("ifstrip", PassId::IFStrip),
    node("demod", PassId::Demod),
    node("detectoraudio", PassId::DetectorAudio),
    node("audiodetector", PassId::DetectorAudio),
    node("receiverinputnetwork", PassId::ReceiverInputNetwork),
    node("detectorload", PassId::ReceiverInputNetwork),
    node("receiverinput", PassId::ReceiverInputNetwork),
    node("receivercircuit", PassId::ReceiverCircuit),
    node("tone", PassId::Tone),
    node("power", PassId::Power),
    node("noise", PassId::Noise),
    node("speaker", PassId::Speaker),
    node("cabinet", PassId::Cabinet),
    node("outputscale", PassId::OutputScale),
    node("finallimiter", PassId::FinalLimiter),
    node("outputclip", PassId::OutputClip),
};

constexpr std::array kSections{
    section("globals", kGlobalsSettings, &validateOversampling),
    section("tuning", kTuningSettings, &validateTuning),
    section("frontend", kFrontEndSettings),
    section("mixer", kMixerSettings),
    section("ifstrip", kIfStripSettings, &validateIfStrip),
    section("demod", kDemodSettings),
    section("receivercircuit", kReceiverCircuitSettings, &validateReceiverInput),
    section("tone", kToneSettings),
    section("power", kPowerSettings),
    section("powersupply", kPowerSupplySettings, &validateThermionic),
    section("noise", kNoiseSettings),
    section("speaker", kSpeakerSettings, &validateSpeaker),
    section("cabinet", kCabinetSettings),
    section("room", kRoomSettings, &validateRoom),
    section("finallimiter", kFinalLimiterSettings),
    section("output", kOutputSettings),
    section("nodes", kNodesSettings),
};

}  // namespace

Schema schema() { return {kSections.data(), kSections.size()}; }

const Section* findSection(std::string_view name) {
  const auto normalized = normalizedName(name);
  for (const auto& section : kSections) {
    if (section.name == normalized) return &section;
  }
  return nullptr;
}

const Field* findField(const Section& section, std::string_view name) {
  const auto normalized = normalizedName(name);
  for (size_t i = 0; i < section.count; ++i) {
    if (normalizedName(section.fields[i].name) == normalized) return &section.fields[i];
  }
  return nullptr;
}

Value read(const Field& field, Radio1938& radio) {
  return std::visit(
      [&](auto target) -> Value {
        if constexpr (std::is_same_v<decltype(target), PassId>)
          return radio.graph.isEnabled(target);
        else
          return target(radio);
      },
      field.target);
}

void assign(const Field& field, Radio1938& radio, const Value& value) {
  std::visit(
      [&](auto target) {
        if constexpr (std::is_same_v<decltype(target), PassId>)
          radio.graph.setEnabled(target, std::get<bool>(value));
        else {
          using T = std::remove_reference_t<decltype(target(radio))>;
          target(radio) = std::get<T>(value);
        }
      },
      field.target);
}

bool validate(const Radio1938& radio, std::string* error) {
  for (const auto& section : kSections) {
    if (section.validate && !section.validate(radio, error)) return false;
  }
  return true;
}

}  // namespace radio_settings
