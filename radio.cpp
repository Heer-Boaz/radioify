#include "radio.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cctype>
#include <complex>
#include <cstdlib>
#include <fstream>
#include <limits>

using BlockStep = Radio1938::BlockStep;
using AllocateStep = Radio1938::AllocateStep;
using ConfigureStep = Radio1938::ConfigureStep;
using InitializeDependentStateStep = Radio1938::InitializeDependentStateStep;
using ProgramPathStep = Radio1938::ProgramPathStep;
using ResetStep = Radio1938::ResetStep;
using SampleControlStep = Radio1938::SampleControlStep;

static inline float clampf(float x, float a, float b) {
  return std::min(std::max(x, a), b);
}

template <typename T>
static inline T requirePositiveFinite(T x) {
  assert(std::isfinite(x) && x > static_cast<T>(0));
  return x;
}

template <typename T>
static inline T requireNonNegativeFinite(T x) {
  assert(std::isfinite(x) && x >= static_cast<T>(0));
  return x;
}

// Moved to radio_settings.cpp
#if 0
static std::string trimIniToken(std::string value) {
  size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(begin, end - begin);
}

static std::string removeIniComments(std::string value) {
  size_t comment = value.find_first_of("#;");
  if (comment == std::string::npos) return value;
  return value.substr(0, comment);
}

static std::string normalizeIniName(std::string value) {
  std::string out;
  out.reserve(value.size());
  for (unsigned char ch : value) {
    if (std::isalnum(ch)) {
      out.push_back(static_cast<char>(std::tolower(ch)));
    }
  }
  return out;
}

static std::string iniError(int line, const std::string& details) {
  return std::string("line ") + std::to_string(line) + ": " + details;
}

static bool parseIniFloat(const std::string& valueText,
                         float& value,
                         std::string* error,
                         int lineNumber,
                         const std::string& settingName) {
  std::string valueTrimmed = trimIniToken(valueText);
  if (valueTrimmed.empty()) {
    if (error) {
      *error = iniError(lineNumber,
                        "setting '" + settingName + "' requires a numeric value");
    }
    return false;
  }
  char* end = nullptr;
  float parsed = std::strtof(valueTrimmed.c_str(), &end);
  if (end == valueTrimmed.c_str() || *end != '\0') {
    if (error) {
      *error =
          iniError(lineNumber, "setting '" + settingName + "' has invalid value");
    }
    return false;
  }
  value = parsed;
  return true;
}

static bool parseIniInt(const std::string& valueText,
                       int& value,
                       std::string* error,
                       int lineNumber,
                       const std::string& settingName) {
  std::string valueTrimmed = trimIniToken(valueText);
  if (valueTrimmed.empty()) {
    if (error) {
      *error = iniError(lineNumber,
                        "setting '" + settingName + "' requires an integer value");
    }
    return false;
  }
  char* end = nullptr;
  long parsed = std::strtol(valueTrimmed.c_str(), &end, 10);
  if (end == valueTrimmed.c_str() || *end != '\0') {
    if (error) {
      *error =
          iniError(lineNumber, "setting '" + settingName + "' has invalid value");
    }
    return false;
  }
  if (parsed < std::numeric_limits<int>::min() ||
      parsed > std::numeric_limits<int>::max()) {
    if (error) {
      *error =
          iniError(lineNumber,
                   "setting '" + settingName + "' is outside int range");
    }
    return false;
  }
  value = static_cast<int>(parsed);
  return true;
}

static bool parseIniBool(const std::string& valueText,
                        bool& value,
                        std::string* error,
                        int lineNumber,
                        const std::string& settingName) {
  std::string valueNorm = normalizeIniName(trimIniToken(valueText));
  if (valueNorm.empty()) {
    if (error) {
      *error = iniError(
          lineNumber,
          "setting '" + settingName + "' requires a boolean value");
    }
    return false;
  }
  if (valueNorm == "1" || valueNorm == "true" || valueNorm == "yes" ||
      valueNorm == "on" || valueNorm == "enabled") {
    value = true;
    return true;
  }
  if (valueNorm == "0" || valueNorm == "false" || valueNorm == "no" ||
      valueNorm == "off" || valueNorm == "disabled") {
    value = false;
    return true;
  }
  if (error) {
    *error = iniError(lineNumber,
                      "setting '" + settingName +
                          "' has invalid boolean value (expected true/false)");
  }
  return false;
}

static bool applyRadioSettingsValue(Radio1938& radio,
                                   const std::string& sectionNorm,
                                   const std::string& keyNorm,
                                   const std::string& valueText,
                                   int lineNumber,
                                   std::string* error) {
  auto setFloat = [&](float& target, const std::string& settingName) {
    float parsed = 0.0f;
    if (!parseIniFloat(valueText, parsed, error, lineNumber, settingName)) {
      return false;
    }
    target = parsed;
    return true;
  };
  auto setInt = [&](int& target, const std::string& settingName) {
    int parsed = 0;
    if (!parseIniInt(valueText, parsed, error, lineNumber, settingName)) {
      return false;
    }
    target = parsed;
    return true;
  };
  auto setBool = [&](bool& target, const std::string& settingName) {
    bool parsed = false;
    if (!parseIniBool(valueText, parsed, error, lineNumber, settingName)) {
      return false;
    }
    target = parsed;
    return true;
  };

  std::string section = sectionNorm;
  if (section == "demodam" || section == "am") section = "demod";
  if (section == "receiver") section = "receivercircuit";
  if (section == "frontend" || section == "front") {
    section = "frontend";
  }

  if (section == "globals" || section == "global") {
    if (keyNorm == "ifnoisemix") {
      return setFloat(radio.globals.ifNoiseMix, "globals.ifNoiseMix");
    }
    if (keyNorm == "inputpad") return setFloat(radio.globals.inputPad,
                                                "globals.inputPad");
    if (keyNorm == "enableautolevel") {
      return setBool(radio.globals.enableAutoLevel, "globals.enableAutoLevel");
    }
    if (keyNorm == "autotargetdb") {
      return setFloat(radio.globals.autoTargetDb, "globals.autoTargetDb");
    }
    if (keyNorm == "automaxboostdb") {
      return setFloat(radio.globals.autoMaxBoostDb, "globals.autoMaxBoostDb");
    }
    if (keyNorm == "outputclipthreshold") {
      return setFloat(radio.globals.outputClipThreshold,
                      "globals.outputClipThreshold");
    }
    if (keyNorm == "oversamplefactor") {
      return setFloat(radio.globals.oversampleFactor, "globals.oversampleFactor");
    }
    if (keyNorm == "oversamplecutofffraction") {
      return setFloat(radio.globals.oversampleCutoffFraction,
                      "globals.oversampleCutoffFraction");
    }
    if (keyNorm == "postnoisemix") {
      return setFloat(radio.globals.postNoiseMix, "globals.postNoiseMix");
    }
    if (keyNorm == "noiseflooramp") {
      return setFloat(radio.globals.noiseFloorAmp, "globals.noiseFloorAmp");
    }
    return false;
  }

  if (section == "tuning") {
    if (keyNorm == "tuneoffsethz") {
      return setFloat(radio.tuning.tuneOffsetHz, "tuning.tuneOffsetHz");
    }
    if (keyNorm == "safebwminhz") {
      return setFloat(radio.tuning.safeBwMinHz, "tuning.safeBwMinHz");
    }
    if (keyNorm == "safebwmaxhz") {
      return setFloat(radio.tuning.safeBwMaxHz, "tuning.safeBwMaxHz");
    }
    if (keyNorm == "prebwscale") {
      return setFloat(radio.tuning.preBwScale, "tuning.preBwScale");
    }
    if (keyNorm == "postbwscale") {
      return setFloat(radio.tuning.postBwScale, "tuning.postBwScale");
    }
    if (keyNorm == "smoothtau") {
      return setFloat(radio.tuning.smoothTau, "tuning.smoothTau");
    }
    if (keyNorm == "updateeps") {
      return setFloat(radio.tuning.updateEps, "tuning.updateEps");
    }
    if (keyNorm == "magnetictuningenabled") {
      return setBool(radio.tuning.magneticTuningEnabled,
                     "tuning.magneticTuningEnabled");
    }
    if (keyNorm == "afccapturehz") {
      return setFloat(radio.tuning.afcCaptureHz, "tuning.afcCaptureHz");
    }
    if (keyNorm == "afcmaxcorrectionhz") {
      return setFloat(radio.tuning.afcMaxCorrectionHz,
                      "tuning.afcMaxCorrectionHz");
    }
    if (keyNorm == "afcdeadband") {
      return setFloat(radio.tuning.afcDeadband, "tuning.afcDeadband");
    }
    if (keyNorm == "afcresponsems") {
      return setFloat(radio.tuning.afcResponseMs, "tuning.afcResponseMs");
    }
    return false;
  }

  if (section == "frontend") {
    if (keyNorm == "inputhphz") {
      return setFloat(radio.frontEnd.inputHpHz, "frontEnd.inputHpHz");
    }
    if (keyNorm == "rfgain") {
      return setFloat(radio.frontEnd.rfGain, "frontEnd.rfGain");
    }
    if (keyNorm == "avcgaindepth") {
      return setFloat(radio.frontEnd.avcGainDepth, "frontEnd.avcGainDepth");
    }
    if (keyNorm == "selectivitypeakhz") {
      return setFloat(radio.frontEnd.selectivityPeakHz,
                      "frontEnd.selectivityPeakHz");
    }
    if (keyNorm == "selectivitypeakq") {
      return setFloat(radio.frontEnd.selectivityPeakQ,
                      "frontEnd.selectivityPeakQ");
    }
    if (keyNorm == "selectivitypeakgaindb") {
      return setFloat(radio.frontEnd.selectivityPeakGainDb,
                      "frontEnd.selectivityPeakGainDb");
    }
    if (keyNorm == "antennainductancehenries") {
      return setFloat(radio.frontEnd.antennaInductanceHenries,
                      "frontEnd.antennaInductanceHenries");
    }
    if (keyNorm == "antennacapacitancefarads") {
      return setFloat(radio.frontEnd.antennaCapacitanceFarads,
                      "frontEnd.antennaCapacitanceFarads");
    }
    if (keyNorm == "antennaseriesresistanceohms") {
      return setFloat(radio.frontEnd.antennaSeriesResistanceOhms,
                      "frontEnd.antennaSeriesResistanceOhms");
    }
    if (keyNorm == "antennaloadresistanceohms") {
      return setFloat(radio.frontEnd.antennaLoadResistanceOhms,
                      "frontEnd.antennaLoadResistanceOhms");
    }
    if (keyNorm == "rfinductancehenries") {
      return setFloat(radio.frontEnd.rfInductanceHenries,
                      "frontEnd.rfInductanceHenries");
    }
    if (keyNorm == "rfcapacitancefarads") {
      return setFloat(radio.frontEnd.rfCapacitanceFarads,
                      "frontEnd.rfCapacitanceFarads");
    }
    if (keyNorm == "rfseriesresistanceohms") {
      return setFloat(radio.frontEnd.rfSeriesResistanceOhms,
                      "frontEnd.rfSeriesResistanceOhms");
    }
    if (keyNorm == "rfloadresistanceohms") {
      return setFloat(radio.frontEnd.rfLoadResistanceOhms,
                      "frontEnd.rfLoadResistanceOhms");
    }
    return false;
  }

  if (section == "mixer") {
    if (keyNorm == "rfgriddrivevolts") {
      return setFloat(radio.mixer.rfGridDriveVolts, "mixer.rfGridDriveVolts");
    }
    if (keyNorm == "logriddrivevolts") {
      return setFloat(radio.mixer.loGridDriveVolts, "mixer.loGridDriveVolts");
    }
    if (keyNorm == "logridbiasvolts") {
      return setFloat(radio.mixer.loGridBiasVolts, "mixer.loGridBiasVolts");
    }
    if (keyNorm == "avcgriddrivevolts") {
      return setFloat(radio.mixer.avcGridDriveVolts,
                      "mixer.avcGridDriveVolts");
    }
    if (keyNorm == "platesupplyvolts") {
      return setFloat(radio.mixer.plateSupplyVolts, "mixer.plateSupplyVolts");
    }
    if (keyNorm == "platedcvolts") {
      return setFloat(radio.mixer.plateDcVolts, "mixer.plateDcVolts");
    }
    if (keyNorm == "platequiescentvolts") {
      return setFloat(radio.mixer.plateQuiescentVolts,
                      "mixer.plateQuiescentVolts");
    }
    if (keyNorm == "screenvolts") {
      return setFloat(radio.mixer.screenVolts, "mixer.screenVolts");
    }
    if (keyNorm == "biasvolts") {
      return setFloat(radio.mixer.biasVolts, "mixer.biasVolts");
    }
    if (keyNorm == "cutoffvolts") {
      return setFloat(radio.mixer.cutoffVolts, "mixer.cutoffVolts");
    }
    if (keyNorm == "modelcutoffvolts") {
      return setFloat(radio.mixer.modelCutoffVolts, "mixer.modelCutoffVolts");
    }
    if (keyNorm == "platecurrentamps") {
      return setFloat(radio.mixer.plateCurrentAmps, "mixer.plateCurrentAmps");
    }
    if (keyNorm == "platequiescentcurrentamps") {
      return setFloat(radio.mixer.plateQuiescentCurrentAmps,
                      "mixer.plateQuiescentCurrentAmps");
    }
    if (keyNorm == "mutualconductancesiemens") {
      return setFloat(radio.mixer.mutualConductanceSiemens,
                      "mixer.mutualConductanceSiemens");
    }
    if (keyNorm == "acloadresistanceohms") {
      return setFloat(radio.mixer.acLoadResistanceOhms,
                      "mixer.acLoadResistanceOhms");
    }
    if (keyNorm == "platekneevolts") {
      return setFloat(radio.mixer.plateKneeVolts, "mixer.plateKneeVolts");
    }
    if (keyNorm == "gridsoftnessvolts") {
      return setFloat(radio.mixer.gridSoftnessVolts, "mixer.gridSoftnessVolts");
    }
    if (keyNorm == "plateresistanceohms") {
      return setFloat(radio.mixer.plateResistanceOhms,
                      "mixer.plateResistanceOhms");
    }
    if (keyNorm == "operatingpointtolerancevolts") {
      return setFloat(radio.mixer.operatingPointToleranceVolts,
                      "mixer.operatingPointToleranceVolts");
    }
    return false;
  }

  if (section == "ifstrip") {
    if (keyNorm == "enabled") return setBool(radio.ifStrip.enabled, "ifStrip.enabled");
    if (keyNorm == "ifminbwhz") {
      return setFloat(radio.ifStrip.ifMinBwHz, "ifStrip.ifMinBwHz");
    }
    if (keyNorm == "stagegain") {
      return setFloat(radio.ifStrip.stageGain, "ifStrip.stageGain");
    }
    if (keyNorm == "avcgaindepth") {
      return setFloat(radio.ifStrip.avcGainDepth, "ifStrip.avcGainDepth");
    }
    if (keyNorm == "ifcenterhz") {
      return setFloat(radio.ifStrip.ifCenterHz, "ifStrip.ifCenterHz");
    }
    if (keyNorm == "primaryinductancehenries") {
      return setFloat(radio.ifStrip.primaryInductanceHenries,
                      "ifStrip.primaryInductanceHenries");
    }
    if (keyNorm == "secondaryinductancehenries") {
      return setFloat(radio.ifStrip.secondaryInductanceHenries,
                      "ifStrip.secondaryInductanceHenries");
    }
    if (keyNorm == "secondaryloadresistanceohms") {
      return setFloat(radio.ifStrip.secondaryLoadResistanceOhms,
                      "ifStrip.secondaryLoadResistanceOhms");
    }
    if (keyNorm == "interstagecouplingcoeff") {
      return setFloat(radio.ifStrip.interstageCouplingCoeff,
                      "ifStrip.interstageCouplingCoeff");
    }
    if (keyNorm == "outputcouplingcoeff") {
      return setFloat(radio.ifStrip.outputCouplingCoeff,
                      "ifStrip.outputCouplingCoeff");
    }
    return false;
  }

  if (section == "demod") {
    if (keyNorm == "audiodiodedrop") {
      return setFloat(radio.demod.am.audioDiodeDrop, "demod.audioDiodeDrop");
    }
    if (keyNorm == "avcdiodedrop") {
      return setFloat(radio.demod.am.avcDiodeDrop, "demod.avcDiodeDrop");
    }
    if (keyNorm == "audiojunctionslopevolts") {
      return setFloat(radio.demod.am.audioJunctionSlopeVolts,
                      "demod.audioJunctionSlopeVolts");
    }
    if (keyNorm == "avcjunctionslopevolts") {
      return setFloat(radio.demod.am.avcJunctionSlopeVolts,
                      "demod.avcJunctionSlopeVolts");
    }
    if (keyNorm == "detectorstoragecapfarads") {
      return setFloat(radio.demod.am.detectorStorageCapFarads,
                      "demod.detectorStorageCapFarads");
    }
    if (keyNorm == "audiochargeresistanceohms") {
      return setFloat(radio.demod.am.audioChargeResistanceOhms,
                      "demod.audioChargeResistanceOhms");
    }
    if (keyNorm == "audiodischargeresistanceohms") {
      return setFloat(radio.demod.am.audioDischargeResistanceOhms,
                      "demod.audioDischargeResistanceOhms");
    }
    if (keyNorm == "avcchargeresistanceohms") {
      return setFloat(radio.demod.am.avcChargeResistanceOhms,
                      "demod.avcChargeResistanceOhms");
    }
    if (keyNorm == "avcdischargeresistanceohms") {
      return setFloat(radio.demod.am.avcDischargeResistanceOhms,
                      "demod.avcDischargeResistanceOhms");
    }
    if (keyNorm == "avcfiltercapfarads") {
      return setFloat(radio.demod.am.avcFilterCapFarads,
                      "demod.avcFilterCapFarads");
    }
    if (keyNorm == "controlvoltageref") {
      return setFloat(radio.demod.am.controlVoltageRef, "demod.controlVoltageRef");
    }
    if (keyNorm == "senselowhz") {
      return setFloat(radio.demod.am.senseLowHz, "demod.senseLowHz");
    }
    if (keyNorm == "sensehighhz") {
      return setFloat(radio.demod.am.senseHighHz, "demod.senseHighHz");
    }
    if (keyNorm == "afcsenselphz") {
      return setFloat(radio.demod.am.afcSenseLpHz, "demod.afcSenseLpHz");
    }
    if (keyNorm == "afcsoffsethz") {
      return setFloat(radio.demod.am.afcLowOffsetHz, "demod.afcLowOffsetHz");
    }
    if (keyNorm == "afchoffsethz") {
      return setFloat(radio.demod.am.afcHighOffsetHz, "demod.afcHighOffsetHz");
    }
    if (keyNorm == "afclowstep") {
      return setFloat(radio.demod.am.afcLowStep, "demod.afcLowStep");
    }
    if (keyNorm == "afchighstep") {
      return setFloat(radio.demod.am.afcHighStep, "demod.afcHighStep");
    }
    if (keyNorm == "afclowphase") {
      return setFloat(radio.demod.am.afcLowPhase, "demod.afcLowPhase");
    }
    if (keyNorm == "afchighphase") {
      return setFloat(radio.demod.am.afcHighPhase, "demod.afcHighPhase");
    }
    return false;
  }

  if (section == "receivercircuit") {
    if (keyNorm == "enabled") {
      return setBool(radio.receiverCircuit.enabled, "receiverCircuit.enabled");
    }
    if (keyNorm == "volumecontrolresistanceohms") {
      return setFloat(radio.receiverCircuit.volumeControlResistanceOhms,
                      "receiverCircuit.volumeControlResistanceOhms");
    }
    if (keyNorm == "volumecontroltapresistanceohms") {
      return setFloat(radio.receiverCircuit.volumeControlTapResistanceOhms,
                      "receiverCircuit.volumeControlTapResistanceOhms");
    }
    if (keyNorm == "volumecontrolposition") {
      return setFloat(radio.receiverCircuit.volumeControlPosition,
                      "receiverCircuit.volumeControlPosition");
    }
    if (keyNorm == "volumecontrolloudnessresistanceohms") {
      return setFloat(radio.receiverCircuit.volumeControlLoudnessResistanceOhms,
                      "receiverCircuit.volumeControlLoudnessResistanceOhms");
    }
    if (keyNorm == "volumecontrolloudnesscapfarads") {
      return setFloat(radio.receiverCircuit.volumeControlLoudnessCapFarads,
                      "receiverCircuit.volumeControlLoudnessCapFarads");
    }
    if (keyNorm == "volumecontroltapvoltage") {
      return setFloat(radio.receiverCircuit.volumeControlTapVoltage,
                      "receiverCircuit.volumeControlTapVoltage");
    }
    if (keyNorm == "couplingcapfarads") {
      return setFloat(radio.receiverCircuit.couplingCapFarads,
                      "receiverCircuit.couplingCapFarads");
    }
    if (keyNorm == "gridleakresistanceohms") {
      return setFloat(radio.receiverCircuit.gridLeakResistanceOhms,
                      "receiverCircuit.gridLeakResistanceOhms");
    }
    if (keyNorm == "tubeplatesupplyvolts") {
      return setFloat(radio.receiverCircuit.tubePlateSupplyVolts,
                      "receiverCircuit.tubePlateSupplyVolts");
    }
    if (keyNorm == "tubeplatedcvolts") {
      return setFloat(radio.receiverCircuit.tubePlateDcVolts,
                      "receiverCircuit.tubePlateDcVolts");
    }
    if (keyNorm == "tubequiescentplatevolts") {
      return setFloat(radio.receiverCircuit.tubeQuiescentPlateVolts,
                      "receiverCircuit.tubeQuiescentPlateVolts");
    }
    if (keyNorm == "tubescreenvolts") {
      return setFloat(radio.receiverCircuit.tubeScreenVolts,
                      "receiverCircuit.tubeScreenVolts");
    }
    if (keyNorm == "tubebiasvolts") {
      return setFloat(radio.receiverCircuit.tubeBiasVolts,
                      "receiverCircuit.tubeBiasVolts");
    }
    if (keyNorm == "tubeplatecurrentamps") {
      return setFloat(radio.receiverCircuit.tubePlateCurrentAmps,
                      "receiverCircuit.tubePlateCurrentAmps");
    }
    if (keyNorm == "tubequiescentplatecurrentamps") {
      return setFloat(radio.receiverCircuit.tubeQuiescentPlateCurrentAmps,
                      "receiverCircuit.tubeQuiescentPlateCurrentAmps");
    }
    if (keyNorm == "tubemutualconductancesiemens") {
      return setFloat(radio.receiverCircuit.tubeMutualConductanceSiemens,
                      "receiverCircuit.tubeMutualConductanceSiemens");
    }
    if (keyNorm == "tubemu") return setFloat(radio.receiverCircuit.tubeMu, "receiverCircuit.tubeMu");
    if (keyNorm == "tubetriodeconnected") {
      return setBool(radio.receiverCircuit.tubeTriodeConnected,
                     "receiverCircuit.tubeTriodeConnected");
    }
    if (keyNorm == "tubeloadresistanceohms") {
      return setFloat(radio.receiverCircuit.tubeLoadResistanceOhms,
                      "receiverCircuit.tubeLoadResistanceOhms");
    }
    if (keyNorm == "tubeplatekneevolts") {
      return setFloat(radio.receiverCircuit.tubePlateKneeVolts,
                      "receiverCircuit.tubePlateKneeVolts");
    }
    if (keyNorm == "tubegridsoftnessvolts") {
      return setFloat(radio.receiverCircuit.tubeGridSoftnessVolts,
                      "receiverCircuit.tubeGridSoftnessVolts");
    }
    if (keyNorm == "tubeplatresistanceohms") {
      return setFloat(radio.receiverCircuit.tubePlateResistanceOhms,
                      "receiverCircuit.tubePlateResistanceOhms");
    }
    if (keyNorm == "operatingpointtolerancevolts") {
      return setFloat(
          radio.receiverCircuit.operatingPointToleranceVolts,
          "receiverCircuit.operatingPointToleranceVolts");
    }
    if (keyNorm == "tubeplatevoltage") {
      return setFloat(radio.receiverCircuit.tubePlateVoltage,
                      "receiverCircuit.tubePlateVoltage");
    }
    return false;
  }

  if (section == "tone") {
    if (keyNorm == "presencehz") {
      return setFloat(radio.tone.presenceHz, "tone.presenceHz");
    }
    if (keyNorm == "presenceq") {
      return setFloat(radio.tone.presenceQ, "tone.presenceQ");
    }
    if (keyNorm == "presencegaindb") {
      return setFloat(radio.tone.presenceGainDb, "tone.presenceGainDb");
    }
    if (keyNorm == "tiltsplithz") {
      return setFloat(radio.tone.tiltSplitHz, "tone.tiltSplitHz");
    }
    return false;
  }

  if (section == "power") {
    if (keyNorm == "sagenv") return setFloat(radio.power.sagEnv, "power.sagEnv");
    if (keyNorm == "sagatk") return setFloat(radio.power.sagAtk, "power.sagAtk");
    if (keyNorm == "sagrel") return setFloat(radio.power.sagRel, "power.sagRel");
    if (keyNorm == "sagstart") return setFloat(radio.power.sagStart, "power.sagStart");
    if (keyNorm == "sagend") return setFloat(radio.power.sagEnd, "power.sagEnd");
    if (keyNorm == "rectifierphase") {
      return setFloat(radio.power.rectifierPhase, "power.rectifierPhase");
    }
    if (keyNorm == "rippledepth") {
      return setFloat(radio.power.rippleDepth, "power.rippleDepth");
    }
    if (keyNorm == "sagattackms") {
      return setFloat(radio.power.sagAttackMs, "power.sagAttackMs");
    }
    if (keyNorm == "sagreleasems") {
      return setFloat(radio.power.sagReleaseMs, "power.sagReleaseMs");
    }
    if (keyNorm == "rectifierminhz") {
      return setFloat(radio.power.rectifierMinHz, "power.rectifierMinHz");
    }
    if (keyNorm == "rippelesecondharmonicmix") {
      return setFloat(radio.power.rippleSecondHarmonicMix,
                      "power.rippleSecondHarmonicMix");
    }
    if (keyNorm == "gainsagperpower") {
      return setFloat(radio.power.gainSagPerPower, "power.gainSagPerPower");
    }
    if (keyNorm == "ripplegainbase") {
      return setFloat(radio.power.rippleGainBase, "power.rippleGainBase");
    }
    if (keyNorm == "ripplegaindepth") {
      return setFloat(radio.power.rippleGainDepth, "power.rippleGainDepth");
    }
    if (keyNorm == "gainmin") return setFloat(radio.power.gainMin, "power.gainMin");
    if (keyNorm == "gainmax") return setFloat(radio.power.gainMax, "power.gainMax");
    if (keyNorm == "supplydrivedepth") {
      return setFloat(radio.power.supplyDriveDepth, "power.supplyDriveDepth");
    }
    if (keyNorm == "supplybiasdepth") {
      return setFloat(radio.power.supplyBiasDepth, "power.supplyBiasDepth");
    }
    if (keyNorm == "postlphz") {
      return setFloat(radio.power.postLpHz, "power.postLpHz");
    }
    if (keyNorm == "gridcouplingcapfarads") {
      return setFloat(radio.power.gridCouplingCapFarads,
                      "power.gridCouplingCapFarads");
    }
    if (keyNorm == "gridleakresistanceohms") {
      return setFloat(radio.power.gridLeakResistanceOhms,
                      "power.gridLeakResistanceOhms");
    }
    if (keyNorm == "driversourceresistanceohms") {
      return setFloat(radio.power.driverSourceResistanceOhms,
                      "power.driverSourceResistanceOhms");
    }
    if (keyNorm == "tubeplatesupplyvolts") {
      return setFloat(radio.power.tubePlateSupplyVolts, "power.tubePlateSupplyVolts");
    }
    if (keyNorm == "tubeplatedcvolts") {
      return setFloat(radio.power.tubePlateDcVolts, "power.tubePlateDcVolts");
    }
    if (keyNorm == "tubequiescentplatevolts") {
      return setFloat(radio.power.tubeQuiescentPlateVolts,
                      "power.tubeQuiescentPlateVolts");
    }
    if (keyNorm == "tubescreenvolts") {
      return setFloat(radio.power.tubeScreenVolts, "power.tubeScreenVolts");
    }
    if (keyNorm == "tubebiasvolts") {
      return setFloat(radio.power.tubeBiasVolts, "power.tubeBiasVolts");
    }
    if (keyNorm == "tubeplatecurrentamps") {
      return setFloat(radio.power.tubePlateCurrentAmps,
                      "power.tubePlateCurrentAmps");
    }
    if (keyNorm == "tubequiescentplatecurrentamps") {
      return setFloat(radio.power.tubeQuiescentPlateCurrentAmps,
                      "power.tubeQuiescentPlateCurrentAmps");
    }
    if (keyNorm == "tubemutualconductancesiemens") {
      return setFloat(radio.power.tubeMutualConductanceSiemens,
                      "power.tubeMutualConductanceSiemens");
    }
    if (keyNorm == "tubemu") return setFloat(radio.power.tubeMu, "power.tubeMu");
    if (keyNorm == "tubetriodeconnected") {
      return setBool(radio.power.tubeTriodeConnected,
                     "power.tubeTriodeConnected");
    }
    if (keyNorm == "tubeacloadresistanceohms") {
      return setFloat(radio.power.tubeAcLoadResistanceOhms,
                      "power.tubeAcLoadResistanceOhms");
    }
    if (keyNorm == "tubeplatekneevolts") {
      return setFloat(radio.power.tubePlateKneeVolts, "power.tubePlateKneeVolts");
    }
    if (keyNorm == "tubegridsoftnessvolts") {
      return setFloat(radio.power.tubeGridSoftnessVolts,
                      "power.tubeGridSoftnessVolts");
    }
    if (keyNorm == "tubegridcurrentresistanceohms") {
      return setFloat(radio.power.tubeGridCurrentResistanceOhms,
                      "power.tubeGridCurrentResistanceOhms");
    }
    if (keyNorm == "tubeplatresistanceohms") {
      return setFloat(radio.power.tubePlateResistanceOhms,
                      "power.tubePlateResistanceOhms");
    }
    if (keyNorm == "operatingpointtolerancevolts") {
      return setFloat(radio.power.operatingPointToleranceVolts,
                      "power.operatingPointToleranceVolts");
    }
    if (keyNorm == "tubeplatevoltage") {
      return setFloat(radio.power.tubePlateVoltage, "power.tubePlateVoltage");
    }
    if (keyNorm == "interstageprimaryleakageinductancehenries") {
      return setFloat(
          radio.power.interstagePrimaryLeakageInductanceHenries,
          "power.interstagePrimaryLeakageInductanceHenries");
    }
    if (keyNorm == "interstagemagnetizinginductancehenries") {
      return setFloat(
          radio.power.interstageMagnetizingInductanceHenries,
          "power.interstageMagnetizingInductanceHenries");
    }
    if (keyNorm == "interstageprimaryresistanceohms") {
      return setFloat(radio.power.interstagePrimaryResistanceOhms,
                      "power.interstagePrimaryResistanceOhms");
    }
    if (keyNorm == "interstageprimarycorelossresistanceohms") {
      return setFloat(
          radio.power.interstagePrimaryCoreLossResistanceOhms,
          "power.interstagePrimaryCoreLossResistanceOhms");
    }
    if (keyNorm == "interstageprimaryshuntcapfarads") {
      return setFloat(radio.power.interstagePrimaryShuntCapFarads,
                      "power.interstagePrimaryShuntCapFarads");
    }
    if (keyNorm == "interstagesecondaryleakageinductancehenries") {
      return setFloat(
          radio.power.interstageSecondaryLeakageInductanceHenries,
          "power.interstageSecondaryLeakageInductanceHenries");
    }
    if (keyNorm == "interstagesecondaryresistanceohms") {
      return setFloat(radio.power.interstageSecondaryResistanceOhms,
                      "power.interstageSecondaryResistanceOhms");
    }
    if (keyNorm == "interstagesecondaryshuntcapfarads") {
      return setFloat(radio.power.interstageSecondaryShuntCapFarads,
                      "power.interstageSecondaryShuntCapFarads");
    }
    if (keyNorm == "interstageintegrationsubsteps") {
      return setInt(radio.power.interstageIntegrationSubsteps,
                    "power.interstageIntegrationSubsteps");
    }
    if (keyNorm == "outputgridleakresistanceohms") {
      return setFloat(radio.power.outputGridLeakResistanceOhms,
                      "power.outputGridLeakResistanceOhms");
    }
    if (keyNorm == "outputgridcurrentresistanceohms") {
      return setFloat(radio.power.outputGridCurrentResistanceOhms,
                      "power.outputGridCurrentResistanceOhms");
    }
    if (keyNorm == "outputtubeplatesupplyvolts") {
      return setFloat(radio.power.outputTubePlateSupplyVolts,
                      "power.outputTubePlateSupplyVolts");
    }
    if (keyNorm == "outputtubeplatedcvolts") {
      return setFloat(radio.power.outputTubePlateDcVolts,
                      "power.outputTubePlateDcVolts");
    }
    if (keyNorm == "outputtubequiescentplatevolts") {
      return setFloat(radio.power.outputTubeQuiescentPlateVolts,
                      "power.outputTubeQuiescentPlateVolts");
    }
    if (keyNorm == "outputtubebiasvolts") {
      return setFloat(radio.power.outputTubeBiasVolts,
                      "power.outputTubeBiasVolts");
    }
    if (keyNorm == "outputtubeplatecurrentamps") {
      return setFloat(radio.power.outputTubePlateCurrentAmps,
                      "power.outputTubePlateCurrentAmps");
    }
    if (keyNorm == "outputtubequiescentplatecurrentamps") {
      return setFloat(radio.power.outputTubeQuiescentPlateCurrentAmps,
                      "power.outputTubeQuiescentPlateCurrentAmps");
    }
    if (keyNorm == "outputtubemutualconductancesiemens") {
      return setFloat(radio.power.outputTubeMutualConductanceSiemens,
                      "power.outputTubeMutualConductanceSiemens");
    }
    if (keyNorm == "outputtubemu") {
      return setFloat(radio.power.outputTubeMu, "power.outputTubeMu");
    }
    if (keyNorm == "outputtubeplatetoplateloadohms") {
      return setFloat(radio.power.outputTubePlateToPlateLoadOhms,
                      "power.outputTubePlateToPlateLoadOhms");
    }
    if (keyNorm == "outputtubeplatekneevolts") {
      return setFloat(radio.power.outputTubePlateKneeVolts,
                      "power.outputTubePlateKneeVolts");
    }
    if (keyNorm == "outputtubegridsoftnessvolts") {
      return setFloat(radio.power.outputTubeGridSoftnessVolts,
                      "power.outputTubeGridSoftnessVolts");
    }
    if (keyNorm == "outputtubeplatresistanceohms") {
      return setFloat(radio.power.outputTubePlateResistanceOhms,
                      "power.outputTubePlateResistanceOhms");
    }
    if (keyNorm == "outputgridavolts") {
      return setFloat(radio.power.outputGridAVolts, "power.outputGridAVolts");
    }
    if (keyNorm == "outputgridbvolts") {
      return setFloat(radio.power.outputGridBVolts, "power.outputGridBVolts");
    }
    if (keyNorm == "outputoperatingpointtolerancevolts") {
      return setFloat(radio.power.outputOperatingPointToleranceVolts,
                      "power.outputOperatingPointToleranceVolts");
    }
    if (keyNorm == "outputtransformerprimaryleakageinductancehenries") {
      return setFloat(
          radio.power.outputTransformerPrimaryLeakageInductanceHenries,
          "power.outputTransformerPrimaryLeakageInductanceHenries");
    }
    if (keyNorm == "outputtransformermagnetizinginductancehenries") {
      return setFloat(radio.power.outputTransformerMagnetizingInductanceHenries,
                      "power.outputTransformerMagnetizingInductanceHenries");
    }
    if (keyNorm == "outputtransformerturnsratioprimarytosecondary") {
      return setFloat(
          radio.power.outputTransformerTurnsRatioPrimaryToSecondary,
          "power.outputTransformerTurnsRatioPrimaryToSecondary");
    }
    if (keyNorm == "outputtransformerprimaryresistanceohms") {
      return setFloat(radio.power.outputTransformerPrimaryResistanceOhms,
                      "power.outputTransformerPrimaryResistanceOhms");
    }
    if (keyNorm == "outputtransformerprimarycorelossresistanceohms") {
      return setFloat(
          radio.power.outputTransformerPrimaryCoreLossResistanceOhms,
          "power.outputTransformerPrimaryCoreLossResistanceOhms");
    }
    if (keyNorm == "outputtransformerprimaryshuntcapfarads") {
      return setFloat(radio.power.outputTransformerPrimaryShuntCapFarads,
                      "power.outputTransformerPrimaryShuntCapFarads");
    }
    if (keyNorm == "outputtransformersecondaryleakageinductancehenries") {
      return setFloat(
          radio.power.outputTransformerSecondaryLeakageInductanceHenries,
          "power.outputTransformerSecondaryLeakageInductanceHenries");
    }
    if (keyNorm == "outputtransformersecondaryresistanceohms") {
      return setFloat(radio.power.outputTransformerSecondaryResistanceOhms,
                      "power.outputTransformerSecondaryResistanceOhms");
    }
    if (keyNorm == "outputtransformersecondaryshuntcapfarads") {
      return setFloat(radio.power.outputTransformerSecondaryShuntCapFarads,
                      "power.outputTransformerSecondaryShuntCapFarads");
    }
    if (keyNorm == "outputtransformerintegrationsubsteps") {
      return setInt(radio.power.outputTransformerIntegrationSubsteps,
                    "power.outputTransformerIntegrationSubsteps");
    }
    if (keyNorm == "outputloadresistanceohms") {
      return setFloat(radio.power.outputLoadResistanceOhms,
                      "power.outputLoadResistanceOhms");
    }
    if (keyNorm == "nominaloutputpowerwatts") {
      return setFloat(radio.power.nominalOutputPowerWatts,
                      "power.nominalOutputPowerWatts");
    }
    return false;
  }

  if (section == "noise") {
    if (keyNorm == "enablehumtone") {
      return setBool(radio.noiseConfig.enableHumTone, "noise.enableHumTone");
    }
    if (keyNorm == "humhzdefault") {
      return setFloat(radio.noiseConfig.humHzDefault, "noise.humHzDefault");
    }
    if (keyNorm == "noiseref") {
      return setFloat(radio.noiseConfig.noiseWeightRef, "noise.noiseWeightRef");
    }
    if (keyNorm == "noiseweightscalemax") {
      return setFloat(radio.noiseConfig.noiseWeightScaleMax,
                      "noise.noiseWeightScaleMax");
    }
    if (keyNorm == "humampscale") {
      return setFloat(radio.noiseConfig.humAmpScale, "noise.humAmpScale");
    }
    if (keyNorm == "crackleampscale") {
      return setFloat(radio.noiseConfig.crackleAmpScale, "noise.crackleAmpScale");
    }
    if (keyNorm == "crackleratescale") {
      return setFloat(radio.noiseConfig.crackleRateScale, "noise.crackleRateScale");
    }
    return false;
  }

  if (error) {
    *error = iniError(lineNumber, "unknown setting '" + section + "." + keyNorm +
                                         "' in INI file");
  }
  return false;
}

bool applyRadioSettingsIni(Radio1938& radio,
                          const std::string& path,
                          std::string* error) {
  if (path.empty()) {
    if (error) *error = "Radio settings path is empty.";
    return false;
  }

  std::ifstream settingsFile(path);
  if (!settingsFile.is_open()) {
    if (error) {
      *error = "Failed to open radio settings file: " + path;
    }
    return false;
  }

  int lineNumber = 0;
  std::string section = "globals";
  std::string line;
  while (std::getline(settingsFile, line)) {
    ++lineNumber;
    line = trimIniToken(removeIniComments(line));
    if (line.empty()) continue;

    if (line.front() == '[' && line.back() == ']') {
      std::string sectionName = trimIniToken(line.substr(1, line.size() - 2));
      if (sectionName.empty()) {
        if (error) {
          *error = iniError(lineNumber, "empty section name");
        }
        return false;
      }
      section = normalizeIniName(sectionName);
      continue;
    }

    size_t separator = line.find('=');
    if (separator == std::string::npos) {
      separator = line.find(':');
    }
    if (separator == std::string::npos) {
      if (error) {
        *error = iniError(lineNumber,
                          "invalid setting line, expected key = value");
      }
      return false;
    }

    std::string key = trimIniToken(line.substr(0, separator));
    if (key.empty()) {
      if (error) {
        *error = iniError(lineNumber, "missing key name");
      }
      return false;
    }

    std::string value = line.substr(separator + 1);
    if (!applyRadioSettingsValue(radio, normalizeIniName(section),
                                normalizeIniName(key), value, lineNumber,
                                error)) {
      return false;
    }
  }

  if (settingsFile.bad()) {
    if (error) {
      *error = "Failed while reading radio settings file: " + path;
    }
    return false;
  }
  return true;
}

#endif // RADIO_SETTINGS_PARSER_MOVED_TO_RADIO_SETTINGS_CPP
static inline float wrapPhase(float phase) {
  while (phase >= kRadioTwoPi) phase -= kRadioTwoPi;
  while (phase < 0.0f) phase += kRadioTwoPi;
  return phase;
}

static inline float computePowerLoadT(
    const Radio1938::PowerNodeState& power) {
  return clampf((power.sagEnv - power.sagStart) /
                    std::max(1e-6f, power.sagEnd - power.sagStart),
                0.0f, 1.0f);
}

static inline float computeRectifierRippleHz(const Radio1938& radio) {
  float mainsHz = std::max(radio.noiseConfig.humHzDefault, 0.0f);
  float rectifierHz = 2.0f * mainsHz;
  if (radio.power.rectifierMinHz > 0.0f) {
    rectifierHz = std::max(rectifierHz, radio.power.rectifierMinHz);
  }
  return rectifierHz;
}

static inline float computeRectifierRippleWave(
    const Radio1938::PowerNodeState& power) {
  float rectified = std::fabs(std::sin(power.rectifierPhase));
  float centered = rectified - (2.0f / kRadioPi);
  centered +=
      power.rippleSecondHarmonicMix * std::sin(2.0f * power.rectifierPhase);
  return centered;
}

static inline float clampPowerSupplyScale(
    const Radio1938::PowerNodeState& power,
    float scale) {
  float low = std::min(power.gainMin, power.gainMax);
  float high = std::max(power.gainMin, power.gainMax);
  if (high > 0.0f) {
    if (low <= 0.0f) low = 0.0f;
    scale = std::clamp(scale, low, high);
  }
  return std::max(scale, 0.0f);
}

static inline float computePowerBranchSupplyScale(const Radio1938& radio,
                                                  float branchDepth) {
  const auto& power = radio.power;
  float powerT = computePowerLoadT(power);
  float scale = 1.0f - power.gainSagPerPower * powerT;
  branchDepth = std::max(branchDepth, 0.0f);
  if (power.rippleDepth > 0.0f && branchDepth > 0.0f) {
    float rippleGain =
        power.rippleDepth * branchDepth *
        (power.rippleGainBase + power.rippleGainDepth * powerT);
    scale *= 1.0f + computeRectifierRippleWave(power) * rippleGain;
  }
  return clampPowerSupplyScale(power, scale);
}

static inline void advanceRectifierRipplePhase(Radio1938& radio) {
  float rectifierHz = computeRectifierRippleHz(radio);
  if (rectifierHz <= 0.0f) return;
  radio.power.rectifierPhase = wrapPhase(
      radio.power.rectifierPhase +
      kRadioTwoPi * (rectifierHz / std::max(radio.sampleRate, 1.0f)));
}

static inline float db2lin(float db) { return std::pow(10.0f, db / 20.0f); }

static inline float lin2db(float x) { return 20.0f * std::log10(std::max(x, kRadioLinDbFloor)); }

static inline float parallelResistance(float a, float b) {
  if (a <= 0.0f) return std::max(b, 0.0f);
  if (b <= 0.0f) return std::max(a, 0.0f);
  return (a * b) / std::max(a + b, 1e-9f);
}

static inline float diodeJunctionRectify(float vIn,
                                         float dropVolts,
                                         float junctionSlopeVolts) {
  float slope = requirePositiveFinite(junctionSlopeVolts);
  float x = (vIn - dropVolts) / slope;
  if (x >= 20.0f) return vIn - dropVolts;
  if (x <= -20.0f) return 0.0f;
  return slope * std::log1p(std::exp(x));
}

static inline float softplusVolts(float x, float softnessVolts) {
  float slope = requirePositiveFinite(softnessVolts);
  float y = x / slope;
  if (y >= 20.0f) return x;
  if (y <= -20.0f) return 0.0f;
  return slope * std::log1p(std::exp(y));
}

static inline double stableLog1pExp(double x) {
  if (x > 50.0) return x;
  if (x < -50.0) return std::exp(x);
  return std::log1p(std::exp(x));
}

static KorenTriodeModel makeKorenTriodeModel(double mu,
                                             double kg1,
                                             double kp,
                                             double kvb) {
  assert(std::isfinite(mu) && mu > 0.0);
  assert(std::isfinite(kg1) && kg1 > 0.0);
  assert(std::isfinite(kp) && kp > 0.0);
  assert(std::isfinite(kvb) && kvb > 0.0);

  KorenTriodeModel m{};
  m.mu = mu;
  m.ex = 1.5;
  m.kg1 = kg1;
  m.kp = kp;
  m.kvb = kvb;
  return m;
}

struct KorenTriodePlateEval {
  double currentAmps = 0.0;
  double conductanceSiemens = 0.0;
};

static double korenTriodePlateCurrent(double vgk,
                                      double vpk,
                                      const KorenTriodeModel& m);
static double korenTriodePlateConductance(double vgk,
                                          double vpk,
                                          const KorenTriodeModel& m);
static KorenTriodePlateEval evaluateKorenTriodePlate(double vgk,
                                                     double vpk,
                                                     const KorenTriodeModel& m);
static KorenTriodePlateEval evaluateKorenTriodePlateRuntime(
    double vgk,
    double vpk,
    const KorenTriodeModel& model,
    const KorenTriodeLut& lut);

static bool solveLinear3x3(float a[3][3], float b[3], float x[3]);

static float deviceTubePlateCurrent(float gridVolts,
                                    float plateVolts,
                                    float screenVolts,
                                    float biasVolts,
                                    float cutoffVolts,
                                    float quiescentPlateVolts,
                                    float quiescentScreenVolts,
                                    float quiescentPlateCurrentAmps,
                                    float mutualConductanceSiemens,
                                    float plateKneeVolts,
                                    float gridSoftnessVolts);

template <typename Fn, typename T>
static T finiteDifferenceDerivative(Fn&& fn, T x, T delta) {
  T safeDelta = requirePositiveFinite(delta);
  T high = fn(x + safeDelta);
  T low = fn(x - safeDelta);
  return (high - low) / (static_cast<T>(2.0) * safeDelta);
}

struct KorenTriodeMetrics {
  double currentAmps = 0.0;
  double gmSiemens = 0.0;
  double gpSiemens = 0.0;
};

static KorenTriodeMetrics evaluateKorenTriodeMetrics(
    double vgkQ,
    double vpkQ,
    const KorenTriodeModel& m) {
  KorenTriodeMetrics metrics{};
  KorenTriodePlateEval plate = evaluateKorenTriodePlate(vgkQ, vpkQ, m);
  metrics.currentAmps = plate.currentAmps;
  metrics.gmSiemens = finiteDifferenceDerivative(
      [&](double vgk) { return korenTriodePlateCurrent(vgk, vpkQ, m); }, vgkQ,
      0.01);
  metrics.gpSiemens = plate.conductanceSiemens;
  return metrics;
}

static KorenTriodeModel fitKorenTriodeModel(double vgkQ,
                                            double vpkQ,
                                            double iqTarget,
                                            double gmTarget,
                                            double mu) {
  assert(std::isfinite(vgkQ));
  assert(std::isfinite(vpkQ) && vpkQ > 0.0);
  assert(std::isfinite(iqTarget) && iqTarget > 0.0);
  assert(std::isfinite(gmTarget) && gmTarget > 0.0);
  assert(std::isfinite(mu) && mu > 0.0);

  double gpTarget = gmTarget / mu;
  double kpInit = 100.0;
  double kvbInit = vpkQ * vpkQ;
  KorenTriodeModel initShape = makeKorenTriodeModel(mu, 1.0, kpInit, kvbInit);
  double initCurrent = korenTriodePlateCurrent(vgkQ, vpkQ, initShape);
  double kg1Init =
      (initCurrent > 0.0)
          ? (initCurrent / iqTarget)
          : (std::pow(vpkQ, initShape.ex) / iqTarget);

  double u[3] = {std::log(kg1Init), std::log(kpInit), std::log(kvbInit)};
  double bestU[3] = {u[0], u[1], u[2]};
  double bestNorm = std::numeric_limits<double>::infinity();
  const double kModelLogMin = std::log(1e-12);
  const double kModelLogMax = std::log(1e12);

  auto clampModelLog = [&](double x) {
    return std::clamp(x, kModelLogMin, kModelLogMax);
  };

  auto evalResiduals = [&](const double params[3], double residuals[3]) {
    double safeParams[3] = {clampModelLog(params[0]), clampModelLog(params[1]),
                            clampModelLog(params[2])};
    KorenTriodeModel model = makeKorenTriodeModel(
        mu, std::exp(safeParams[0]), std::exp(safeParams[1]),
        std::exp(safeParams[2]));
    KorenTriodeMetrics metrics = evaluateKorenTriodeMetrics(vgkQ, vpkQ, model);
    residuals[0] = metrics.currentAmps / iqTarget - 1.0;
    residuals[1] = metrics.gmSiemens / gmTarget - 1.0;
    residuals[2] = metrics.gpSiemens / gpTarget - 1.0;
  };

  for (int iter = 0; iter < 16; ++iter) {
    double f[3] = {};
    evalResiduals(u, f);
    double norm = std::fabs(f[0]) + std::fabs(f[1]) + std::fabs(f[2]);
    if (norm < bestNorm) {
      bestNorm = norm;
      bestU[0] = u[0];
      bestU[1] = u[1];
      bestU[2] = u[2];
    }
    if (norm < 1e-8) break;

    float a[3][3] = {};
    float b[3] = {-static_cast<float>(f[0]), -static_cast<float>(f[1]),
                  -static_cast<float>(f[2])};
    for (int col = 0; col < 3; ++col) {
      double up[3] = {u[0], u[1], u[2]};
      double um[3] = {u[0], u[1], u[2]};
      up[col] += 1e-3;
      um[col] -= 1e-3;
      double fp[3] = {};
      double fm[3] = {};
      evalResiduals(up, fp);
      evalResiduals(um, fm);
      for (int row = 0; row < 3; ++row) {
        a[row][col] = static_cast<float>((fp[row] - fm[row]) / 2e-3);
      }
    }

    float delta[3] = {};
    if (!solveLinear3x3(a, b, delta)) break;

    double lambda = 1.0;
    double candidateBestNorm = bestNorm;
    double candidateBestU[3] = {u[0], u[1], u[2]};
    for (int ls = 0; ls < 8; ++ls) {
      double candU[3] = {u[0] + lambda * delta[0], u[1] + lambda * delta[1],
                         u[2] + lambda * delta[2]};
      double candF[3] = {};
      evalResiduals(candU, candF);
      double candNorm =
          std::fabs(candF[0]) + std::fabs(candF[1]) + std::fabs(candF[2]);
      if (candNorm < candidateBestNorm) {
        candidateBestNorm = candNorm;
        candidateBestU[0] = candU[0];
        candidateBestU[1] = candU[1];
        candidateBestU[2] = candU[2];
      }
      lambda *= 0.5;
    }

    u[0] = clampModelLog(candidateBestU[0]);
    u[1] = clampModelLog(candidateBestU[1]);
    u[2] = clampModelLog(candidateBestU[2]);

    double maxDelta =
        std::max(std::fabs(delta[0]),
                 std::max(std::fabs(delta[1]), std::fabs(delta[2])));
    if (maxDelta < 1e-6) break;
  }

  return makeKorenTriodeModel(mu, std::exp(bestU[0]), std::exp(bestU[1]),
                              std::exp(bestU[2]));
}

static float fitPentodeModelCutoffVolts(float biasVolts,
                                        float plateVolts,
                                        float screenVolts,
                                        float plateCurrentAmps,
                                        float mutualConductanceSiemens,
                                        float plateKneeVolts,
                                        float gridSoftnessVolts) {
  float safePlateVolts = std::max(plateVolts, 1.0f);
  float safeScreenVolts = std::max(screenVolts, 1.0f);
  float safePlateCurrent = std::max(plateCurrentAmps, 1e-6f);
  float searchMin = biasVolts - 80.0f;
  float searchMax = biasVolts + 6.0f;
  float bestCutoff = searchMin;
  float bestError = std::numeric_limits<float>::infinity();
  constexpr int kSteps = 256;
  for (int i = 0; i <= kSteps; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(kSteps);
    float cutoffVolts = searchMin + (searchMax - searchMin) * t;
    float gmEstimate = finiteDifferenceDerivative(
        [&](float gridVolts) {
          return deviceTubePlateCurrent(
              gridVolts, safePlateVolts, safeScreenVolts, biasVolts,
              cutoffVolts, safePlateVolts, safeScreenVolts, safePlateCurrent,
              mutualConductanceSiemens, plateKneeVolts, gridSoftnessVolts);
        },
        biasVolts, 0.02f);
    float error = std::fabs(gmEstimate - mutualConductanceSiemens);
    if (error < bestError) {
      bestError = error;
      bestCutoff = cutoffVolts;
    }
  }
  return bestCutoff;
}

template <typename PlateCurrentFn>
static float solveLoadLinePlateVoltage(float plateSupplyVolts,
                                       float loadResistanceOhms,
                                       float initialGuessVolts,
                                       PlateCurrentFn&& plateCurrentForPlate) {
  float safeSupply = requirePositiveFinite(plateSupplyVolts);
  float safeLoadResistance = requirePositiveFinite(loadResistanceOhms);

  auto residual = [&](float plateVolts) {
    float safePlate = clampf(plateVolts, 0.0f, safeSupply);
    return safeSupply - plateCurrentForPlate(safePlate) * safeLoadResistance -
           safePlate;
  };

  float bestPlate = clampf(initialGuessVolts, 0.0f, safeSupply);
  float bestResidual = residual(bestPlate);
  float low = 0.0f;
  float high = safeSupply;
  float lowResidual = residual(low);
  float highResidual = residual(high);
  if (std::fabs(lowResidual) < std::fabs(bestResidual)) {
    bestPlate = low;
    bestResidual = lowResidual;
  }
  if (std::fabs(highResidual) < std::fabs(bestResidual)) {
    bestPlate = high;
    bestResidual = highResidual;
  }

  if (lowResidual * highResidual <= 0.0f) {
    for (int i = 0; i < 24; ++i) {
      float mid = 0.5f * (low + high);
      float midResidual = residual(mid);
      if (std::fabs(midResidual) < std::fabs(bestResidual)) {
        bestPlate = mid;
        bestResidual = midResidual;
      }
      if (midResidual == 0.0f) break;
      if (lowResidual * midResidual <= 0.0f) {
        high = mid;
        highResidual = midResidual;
      } else {
        low = mid;
        lowResidual = midResidual;
      }
    }
    return bestPlate;
  }

  float plateVolts = bestPlate;
  for (int i = 0; i < 16; ++i) {
    float targetPlate = safeSupply - plateCurrentForPlate(plateVolts) *
                                         safeLoadResistance;
    targetPlate = clampf(targetPlate, 0.0f, safeSupply);
    plateVolts = 0.5f * (plateVolts + targetPlate);
  }
  return plateVolts;
}

static TriodeOperatingPoint solveTriodeOperatingPoint(
    float plateSupplyVolts,
    float loadResistanceOhms,
    float biasVolts,
    float targetPlateVolts,
    float targetPlateCurrentAmps,
    float targetMutualConductanceSiemens,
    float mu) {
  KorenTriodeModel model = fitKorenTriodeModel(
      biasVolts, targetPlateVolts, targetPlateCurrentAmps,
      targetMutualConductanceSiemens, mu);
  float solvedPlateVolts = solveLoadLinePlateVoltage(
      plateSupplyVolts, loadResistanceOhms, targetPlateVolts,
      [&](float plateVolts) {
        return static_cast<float>(
            korenTriodePlateCurrent(biasVolts, plateVolts, model));
      });
  float solvedPlateCurrent = static_cast<float>(
      korenTriodePlateCurrent(biasVolts, solvedPlateVolts, model));
  float plateSlope = finiteDifferenceDerivative(
      [&](float plateVolts) {
        return static_cast<float>(
            korenTriodePlateCurrent(biasVolts, plateVolts, model));
      },
      solvedPlateVolts, 0.5f);
  float rpOhms = 1.0f / std::max(std::fabs(plateSlope), 1e-9f);
  return TriodeOperatingPoint{solvedPlateVolts, solvedPlateCurrent, rpOhms,
                              model};
}

static PentodeOperatingPoint solvePentodeOperatingPoint(
    float plateSupplyVolts,
    float screenVolts,
    float loadResistanceOhms,
    float biasVolts,
    float targetPlateVolts,
    float targetPlateCurrentAmps,
    float targetMutualConductanceSiemens,
    float plateKneeVolts,
    float gridSoftnessVolts,
    float& fittedCutoffVolts) {
  auto solveOnce = [&](float referencePlateVolts,
                       float referencePlateCurrentAmps) {
    float safeReferencePlate = std::max(referencePlateVolts, 1.0f);
    float safeReferenceCurrent = std::max(referencePlateCurrentAmps, 1e-6f);
    fittedCutoffVolts = fitPentodeModelCutoffVolts(
        biasVolts, safeReferencePlate, screenVolts, safeReferenceCurrent,
        targetMutualConductanceSiemens, plateKneeVolts, gridSoftnessVolts);
    float solvedPlateVolts = solveLoadLinePlateVoltage(
        plateSupplyVolts, loadResistanceOhms, safeReferencePlate,
        [&](float plateVolts) {
          return deviceTubePlateCurrent(
              biasVolts, plateVolts, screenVolts, biasVolts, fittedCutoffVolts,
              safeReferencePlate, screenVolts, safeReferenceCurrent,
              targetMutualConductanceSiemens, plateKneeVolts,
              gridSoftnessVolts);
        });
    float solvedPlateCurrent = deviceTubePlateCurrent(
        biasVolts, solvedPlateVolts, screenVolts, biasVolts, fittedCutoffVolts,
        safeReferencePlate, screenVolts, safeReferenceCurrent,
        targetMutualConductanceSiemens, plateKneeVolts, gridSoftnessVolts);
    float plateSlope = finiteDifferenceDerivative(
        [&](float plateVolts) {
          return deviceTubePlateCurrent(
              biasVolts, std::max(plateVolts, 0.0f), screenVolts, biasVolts,
              fittedCutoffVolts, solvedPlateVolts, screenVolts,
              std::max(solvedPlateCurrent, 1e-6f),
              targetMutualConductanceSiemens, plateKneeVolts,
              gridSoftnessVolts);
        },
        solvedPlateVolts, 0.5f);
    float rpOhms = 1.0f / std::max(std::fabs(plateSlope), 1e-9f);
    return PentodeOperatingPoint{solvedPlateVolts, solvedPlateCurrent, rpOhms};
  };

  PentodeOperatingPoint solved =
      solveOnce(targetPlateVolts, targetPlateCurrentAmps);
  solved = solveOnce(solved.plateVolts, solved.plateCurrentAmps);
  return solved;
}

static float tubeGridBranchCurrent(float acGridVolts,
                                   float biasVolts,
                                   float gridLeakResistanceOhms,
                                   float gridCurrentResistanceOhms) {
  float leakCurrent = acGridVolts / gridLeakResistanceOhms;
  float positiveGridCurrent =
      (biasVolts + acGridVolts > 0.0f)
          ? ((biasVolts + acGridVolts) / gridCurrentResistanceOhms)
          : 0.0f;
  return leakCurrent + positiveGridCurrent;
}

static float tubeGridBranchSlope(float acGridVolts,
                                 float biasVolts,
                                 float gridLeakResistanceOhms,
                                 float gridCurrentResistanceOhms) {
  float slope = 1.0f / gridLeakResistanceOhms;
  if (biasVolts + acGridVolts > 0.0f) {
    slope += 1.0f / gridCurrentResistanceOhms;
  }
  return slope;
}

static float solveCapCoupledGridVoltage(float sourceVolts,
                                        float previousCapVoltage,
                                        float dt,
                                        float couplingCapFarads,
                                        float sourceResistanceOhms,
                                        float gridLeakResistanceOhms,
                                        float biasVolts,
                                        float gridCurrentResistanceOhms) {
  float seriesTerm = requirePositiveFinite(sourceResistanceOhms) +
                     dt / requirePositiveFinite(couplingCapFarads);

  float leakConductance = 1.0f / requirePositiveFinite(gridLeakResistanceOhms);
  float gridCurrentConductance =
      1.0f / requirePositiveFinite(gridCurrentResistanceOhms);

  float gridVolts = sourceVolts - previousCapVoltage;

  for (int i = 0; i < 8; ++i) {
    float controlGridVolts = biasVolts + gridVolts;
    float positiveGridCurrent =
        (controlGridVolts > 0.0f) ? controlGridVolts * gridCurrentConductance
                                  : 0.0f;
    float positiveGridSlope =
        (controlGridVolts > 0.0f) ? gridCurrentConductance : 0.0f;

    float f = (sourceVolts - previousCapVoltage - gridVolts) / seriesTerm -
              leakConductance * gridVolts - positiveGridCurrent;

    float df = -1.0f / seriesTerm - leakConductance - positiveGridSlope;

    assert(std::fabs(df) > 1e-9f);
    gridVolts -= f / df;
  }

  return gridVolts;
}

static float scalePhysicalSpeakerVoltsToDigital(const Radio1938& radio,
                                                float speakerVolts) {
  if (!radio.graph.isEnabled(StageId::Power)) return speakerVolts;
  // The modeled output-stage reference is a clean-power truth value. Leave a
  // small digital peak margin above that clean reference so real program crest
  // factor, cabinet lift, and modest user volume boosts do not immediately hit
  // the post-scaling safety clipper.
  constexpr float kDigitalProgramPeakHeadroom = 1.12f;
  return speakerVolts /
         (requirePositiveFinite(radio.output.digitalReferenceSpeakerVoltsPeak) *
          kDigitalProgramPeakHeadroom);
}

static float applyDigitalMakeupGain(const Radio1938& radio, float yDigital) {
  if (!radio.graph.isEnabled(StageId::Power)) return yDigital;
  return yDigital * std::max(radio.output.digitalMakeupGain, 0.0f);
}

static float deviceTubePlateCurrent(float gridVolts,
                                    float plateVolts,
                                    float screenVolts,
                                    float biasVolts,
                                    float cutoffVolts,
                                    float quiescentPlateVolts,
                                    float quiescentScreenVolts,
                                    float quiescentPlateCurrentAmps,
                                    float mutualConductanceSiemens,
                                    float plateKneeVolts,
                                    float gridSoftnessVolts) {
  float activationQ =
      softplusVolts(biasVolts - cutoffVolts, gridSoftnessVolts);
  float activation =
      softplusVolts(gridVolts - cutoffVolts, gridSoftnessVolts);
  activationQ = std::max(activationQ, 1e-5f);
  activation = std::max(activation, 0.0f);
  float exponent = clampf(mutualConductanceSiemens * activationQ /
                              requirePositiveFinite(quiescentPlateCurrentAmps),
                          1.05f, 10.0f);
  float kneeQ =
      1.0f - std::exp(-std::max(quiescentPlateVolts, 0.0f) /
                      requirePositiveFinite(plateKneeVolts));
  float knee =
      1.0f - std::exp(-std::max(plateVolts, 0.0f) /
                      requirePositiveFinite(plateKneeVolts));
  float screenScale = screenVolts / requirePositiveFinite(quiescentScreenVolts);
  float perveance =
      quiescentPlateCurrentAmps /
      (std::pow(activationQ, exponent) * std::max(kneeQ, 1e-6f));
  return perveance * std::pow(activation, exponent) *
         std::max(knee, 0.0f) * std::max(screenScale, 0.0f);
}

static float processResistorLoadedTubeStage(float gridVolts,
                                            float supplyScale,
                                            float plateSupplyVolts,
                                            float quiescentPlateVolts,
                                            float screenVolts,
                                            float biasVolts,
                                            float cutoffVolts,
                                            float quiescentPlateCurrentAmps,
                                            float mutualConductanceSiemens,
                                            float loadResistanceOhms,
                                            float plateKneeVolts,
                                            float gridSoftnessVolts,
                                            float& plateVoltageState) {
  float supply = requirePositiveFinite(plateSupplyVolts * supplyScale);
  float screen = requirePositiveFinite(screenVolts * supplyScale);
  float safeLoadResistance = requirePositiveFinite(loadResistanceOhms);
  float plateVoltage = (plateVoltageState > 0.0f)
                           ? plateVoltageState
                           : std::clamp(quiescentPlateVolts * supplyScale, 0.0f,
                                        supply);
  for (int i = 0; i < 4; ++i) {
    float current = deviceTubePlateCurrent(
        gridVolts, plateVoltage, screen, biasVolts, cutoffVolts,
        quiescentPlateVolts * supplyScale, screenVolts * supplyScale,
        quiescentPlateCurrentAmps, mutualConductanceSiemens, plateKneeVolts,
        gridSoftnessVolts);
    float targetPlate =
        std::clamp(supply - current * safeLoadResistance, 0.0f, supply);
    plateVoltage = 0.5f * (plateVoltage + targetPlate);
  }
  plateVoltageState = plateVoltage;
  float quiescentPlate = std::clamp(quiescentPlateVolts * supplyScale, 0.0f, supply);
  return quiescentPlate - plateVoltage;
}

static float processResistorLoadedTriodeStage(
    float gridVolts,
    float supplyScale,
    float plateSupplyVolts,
    float quiescentPlateVolts,
    const KorenTriodeModel& model,
    const KorenTriodeLut* lut,
    float loadResistanceOhms,
    float& plateVoltageState) {
  float supply = requirePositiveFinite(plateSupplyVolts * supplyScale);
  float safeLoadResistance = requirePositiveFinite(loadResistanceOhms);
  float plateVoltage = (plateVoltageState > 0.0f)
                           ? plateVoltageState
                           : std::clamp(quiescentPlateVolts * supplyScale, 0.0f,
                                        supply);
  for (int i = 0; i < 4; ++i) {
    KorenTriodePlateEval eval =
        (lut != nullptr)
            ? evaluateKorenTriodePlateRuntime(gridVolts, plateVoltage, model,
                                              *lut)
            : evaluateKorenTriodePlate(gridVolts, plateVoltage, model);
    float current = static_cast<float>(eval.currentAmps);
    float targetPlate =
        std::clamp(supply - current * safeLoadResistance, 0.0f, supply);
    plateVoltage = 0.5f * (plateVoltage + targetPlate);
  }
  plateVoltageState = plateVoltage;
  float quiescentPlate = std::clamp(quiescentPlateVolts * supplyScale, 0.0f, supply);
  return quiescentPlate - plateVoltage;
}

struct FixedPlatePentodeEvaluator {
  float cutoffVolts = 0.0f;
  float gridSoftnessVolts = 0.0f;
  float exponent = 1.0f;
  float currentScale = 0.0f;

  float eval(float gridVolts) const {
    float activation =
        std::max(softplusVolts(gridVolts - cutoffVolts, gridSoftnessVolts), 0.0f);
    return currentScale * std::pow(activation, exponent);
  }

  float evalDerivative(float gridVolts) const {
    float slope = requirePositiveFinite(gridSoftnessVolts);
    float y = (gridVolts - cutoffVolts) / slope;
    float activation = 0.0f;
    float logistic = 0.0f;
    if (y >= 20.0f) {
      activation = gridVolts - cutoffVolts;
      logistic = 1.0f;
    } else if (y <= -20.0f) {
      activation = 0.0f;
      logistic = 0.0f;
    } else {
      activation = slope * std::log1p(std::exp(y));
      logistic = 1.0f / (1.0f + std::exp(-y));
    }
    if (activation <= 0.0f) {
      return 0.0f;
    }
    return currentScale * exponent * std::pow(activation, exponent - 1.0f) *
           logistic;
  }
};

static FixedPlatePentodeEvaluator prepareFixedPlatePentodeEvaluator(
    float plateVolts,
    float screenVolts,
    float biasVolts,
    float cutoffVolts,
    float quiescentPlateVolts,
    float quiescentScreenVolts,
    float quiescentPlateCurrentAmps,
    float mutualConductanceSiemens,
    float plateKneeVolts,
    float gridSoftnessVolts) {
  float activationQ =
      std::max(softplusVolts(biasVolts - cutoffVolts, gridSoftnessVolts), 1e-5f);
  float exponent = clampf(mutualConductanceSiemens * activationQ /
                              requirePositiveFinite(quiescentPlateCurrentAmps),
                          1.05f, 10.0f);
  float knee =
      1.0f - std::exp(-std::max(plateVolts, 0.0f) /
                      requirePositiveFinite(plateKneeVolts));
  float kneeQ =
      1.0f - std::exp(-std::max(quiescentPlateVolts, 0.0f) /
                      requirePositiveFinite(plateKneeVolts));
  float screenScale = screenVolts / requirePositiveFinite(quiescentScreenVolts);
  float perveance =
      quiescentPlateCurrentAmps /
      (std::pow(activationQ, exponent) * std::max(kneeQ, 1e-6f));

  FixedPlatePentodeEvaluator out{};
  out.cutoffVolts = cutoffVolts;
  out.gridSoftnessVolts = gridSoftnessVolts;
  out.exponent = exponent;
  out.currentScale =
      perveance * std::max(knee, 0.0f) * std::max(screenScale, 0.0f);
  return out;
}

struct ConverterTubeStageResult {
  float plateAcVolts = 0.0f;
  float mixedPlateCurrent = 0.0f;
};

static ConverterTubeStageResult processConverterTubeStage(float rfGridVolts,
                                                          float mixedBaseGridVolts,
                                                          float oscillatorGridVolts,
                                                          const FixedPlatePentodeEvaluator& plateCurrentForGrid,
                                                          float acLoadResistanceOhms,
                                                          float& mixedCurrentOut) {
  float idleCurrent = plateCurrentForGrid.eval(mixedBaseGridVolts);
  float rfOnlyCurrent =
      plateCurrentForGrid.eval(mixedBaseGridVolts + rfGridVolts);
  float loOnlyCurrent =
      plateCurrentForGrid.eval(mixedBaseGridVolts + oscillatorGridVolts);
  float mixedCurrent = plateCurrentForGrid.eval(mixedBaseGridVolts + rfGridVolts +
                                                oscillatorGridVolts);
  float conversionCurrent =
      mixedCurrent - rfOnlyCurrent - loOnlyCurrent + idleCurrent;
  mixedCurrentOut = mixedCurrent;
  ConverterTubeStageResult out{};
  out.plateAcVolts =
      conversionCurrent * requirePositiveFinite(acLoadResistanceOhms);
  out.mixedPlateCurrent = mixedCurrent;
  return out;
}

static bool solveLinear3x3(float a[3][3], float b[3], float x[3]) {
  for (int pivot = 0; pivot < 3; ++pivot) {
    int pivotRow = pivot;
    float pivotAbs = std::fabs(a[pivot][pivot]);
    for (int row = pivot + 1; row < 3; ++row) {
      float candidateAbs = std::fabs(a[row][pivot]);
      if (candidateAbs > pivotAbs) {
        pivotAbs = candidateAbs;
        pivotRow = row;
      }
    }
    if (pivotAbs < 1e-12f) return false;
    if (pivotRow != pivot) {
      for (int col = pivot; col < 3; ++col) std::swap(a[pivot][col], a[pivotRow][col]);
      std::swap(b[pivot], b[pivotRow]);
    }
    float invPivot = 1.0f / a[pivot][pivot];
    for (int row = pivot + 1; row < 3; ++row) {
      float scale = a[row][pivot] * invPivot;
      if (std::fabs(scale) < 1e-12f) continue;
      for (int col = pivot; col < 3; ++col) a[row][col] -= scale * a[pivot][col];
      b[row] -= scale * b[pivot];
    }
  }
  for (int row = 2; row >= 0; --row) {
    float sum = b[row];
    for (int col = row + 1; col < 3; ++col) sum -= a[row][col] * x[col];
    if (std::fabs(a[row][row]) < 1e-12f) return false;
    x[row] = sum / a[row][row];
  }
  return true;
}

static bool solveLinear4x4(float a[4][4], float b[4], float x[4]) {
  for (int pivot = 0; pivot < 4; ++pivot) {
    int pivotRow = pivot;
    float pivotAbs = std::fabs(a[pivot][pivot]);
    for (int row = pivot + 1; row < 4; ++row) {
      float candidateAbs = std::fabs(a[row][pivot]);
      if (candidateAbs > pivotAbs) {
        pivotAbs = candidateAbs;
        pivotRow = row;
      }
    }
    if (pivotAbs < 1e-12f) return false;
    if (pivotRow != pivot) {
      for (int col = pivot; col < 4; ++col) {
        std::swap(a[pivot][col], a[pivotRow][col]);
      }
      std::swap(b[pivot], b[pivotRow]);
    }
    float invPivot = 1.0f / a[pivot][pivot];
    for (int row = pivot + 1; row < 4; ++row) {
      float scale = a[row][pivot] * invPivot;
      if (std::fabs(scale) < 1e-12f) continue;
      for (int col = pivot; col < 4; ++col) {
        a[row][col] -= scale * a[pivot][col];
      }
      b[row] -= scale * b[pivot];
    }
  }
  for (int row = 3; row >= 0; --row) {
    float sum = b[row];
    for (int col = row + 1; col < 4; ++col) sum -= a[row][col] * x[col];
    if (std::fabs(a[row][row]) < 1e-12f) return false;
    x[row] = sum / a[row][row];
  }
  return true;
}

static double korenTriodePlateCurrent(double vgk,
                                      double vpk,
                                      const KorenTriodeModel& m) {
  return evaluateKorenTriodePlate(vgk, vpk, m).currentAmps;
}

static double korenTriodePlateConductance(double vgk,
                                          double vpk,
                                          const KorenTriodeModel& m) {
  return evaluateKorenTriodePlate(vgk, vpk, m).conductanceSiemens;
}

static KorenTriodePlateEval evaluateKorenTriodePlate(
    double vgk,
    double vpk,
    const KorenTriodeModel& m) {
  KorenTriodePlateEval eval{};
  if (vpk <= 0.0) {
    return eval;
  }

  double sqrtTermSq = m.kvb + vpk * vpk;
  double sqrtTerm = std::sqrt(sqrtTermSq);
  double term = (1.0 / m.mu) + vgk / sqrtTerm;
  double activation = m.kp * term;
  double logTerm = stableLog1pExp(activation);
  double e1 = (vpk / m.kp) * logTerm;
  if (e1 <= 0.0) {
    return eval;
  }

  eval.currentAmps = std::pow(e1, m.ex) / m.kg1;

  double sigmoid = 0.0;
  if (activation >= 50.0) {
    sigmoid = 1.0;
  } else if (activation <= -50.0) {
    sigmoid = std::exp(activation);
  } else {
    sigmoid = 1.0 / (1.0 + std::exp(-activation));
  }

  double sqrtTermCubed = sqrtTermSq * sqrtTerm;
  double dE1dVpk =
      (logTerm / m.kp) -
      (vgk * vpk * vpk * sigmoid) / std::max(sqrtTermCubed, 1e-18);
  eval.conductanceSiemens =
      eval.currentAmps * (m.ex * dE1dVpk / std::max(e1, 1e-18));
  return eval;
}

static void configureKorenTriodeLut(KorenTriodeLut& lut,
                                    const KorenTriodeModel& model,
                                    float vgkMin,
                                    float vgkMax,
                                    int vgkBins,
                                    float vpkMin,
                                    float vpkMax,
                                    int vpkBins) {
  lut.vgkMin = vgkMin;
  lut.vgkMax = vgkMax;
  lut.vpkMin = vpkMin;
  lut.vpkMax = vpkMax;
  lut.vgkBins = std::max(vgkBins, 2);
  lut.vpkBins = std::max(vpkBins, 2);
  lut.vgkInvStep = static_cast<float>(lut.vgkBins - 1) /
                   std::max(lut.vgkMax - lut.vgkMin, 1e-6f);
  lut.vpkInvStep = static_cast<float>(lut.vpkBins - 1) /
                   std::max(lut.vpkMax - lut.vpkMin, 1e-6f);
  size_t sampleCount =
      static_cast<size_t>(lut.vgkBins) * static_cast<size_t>(lut.vpkBins);
  lut.currentAmps.resize(sampleCount);
  lut.conductanceSiemens.resize(sampleCount);

  for (int y = 0; y < lut.vpkBins; ++y) {
    float vpk = lut.vpkMin +
                static_cast<float>(y) /
                    static_cast<float>(lut.vpkBins - 1) *
                    (lut.vpkMax - lut.vpkMin);
    for (int x = 0; x < lut.vgkBins; ++x) {
      float vgk = lut.vgkMin +
                  static_cast<float>(x) /
                      static_cast<float>(lut.vgkBins - 1) *
                      (lut.vgkMax - lut.vgkMin);
      KorenTriodePlateEval eval =
          evaluateKorenTriodePlate(vgk, vpk, model);
      size_t index =
          static_cast<size_t>(y) * static_cast<size_t>(lut.vgkBins) +
          static_cast<size_t>(x);
      lut.currentAmps[index] = static_cast<float>(eval.currentAmps);
      lut.conductanceSiemens[index] =
          static_cast<float>(eval.conductanceSiemens);
    }
  }
}

static KorenTriodePlateEval evaluateKorenTriodePlateLut(
    double vgk,
    double vpk,
    const KorenTriodeLut& lut) {
  if (!lut.valid()) {
    return {};
  }

  float clampedVgk =
      std::clamp(static_cast<float>(vgk), lut.vgkMin, lut.vgkMax);
  float clampedVpk =
      std::clamp(static_cast<float>(vpk), lut.vpkMin, lut.vpkMax);
  float x = (clampedVgk - lut.vgkMin) * lut.vgkInvStep;
  float y = (clampedVpk - lut.vpkMin) * lut.vpkInvStep;
  int x0 = std::clamp(static_cast<int>(x), 0, lut.vgkBins - 2);
  int y0 = std::clamp(static_cast<int>(y), 0, lut.vpkBins - 2);
  float tx = x - static_cast<float>(x0);
  float ty = y - static_cast<float>(y0);
  int x1 = x0 + 1;
  int y1 = y0 + 1;

  auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
  auto sampleAt = [&](const std::vector<float>& values, int xi, int yi) {
    size_t index =
        static_cast<size_t>(yi) * static_cast<size_t>(lut.vgkBins) +
        static_cast<size_t>(xi);
    return values[index];
  };

  float i00 = sampleAt(lut.currentAmps, x0, y0);
  float i10 = sampleAt(lut.currentAmps, x1, y0);
  float i01 = sampleAt(lut.currentAmps, x0, y1);
  float i11 = sampleAt(lut.currentAmps, x1, y1);
  float g00 = sampleAt(lut.conductanceSiemens, x0, y0);
  float g10 = sampleAt(lut.conductanceSiemens, x1, y0);
  float g01 = sampleAt(lut.conductanceSiemens, x0, y1);
  float g11 = sampleAt(lut.conductanceSiemens, x1, y1);

  KorenTriodePlateEval eval{};
  eval.currentAmps = lerp(lerp(i00, i10, tx), lerp(i01, i11, tx), ty);
  eval.conductanceSiemens = lerp(lerp(g00, g10, tx), lerp(g01, g11, tx), ty);
  return eval;
}

static KorenTriodePlateEval evaluateKorenTriodePlateRuntime(
    double vgk,
    double vpk,
    const KorenTriodeModel& model,
    const KorenTriodeLut& lut) {
  if (lut.valid()) {
    return evaluateKorenTriodePlateLut(vgk, vpk, lut);
  }
  return evaluateKorenTriodePlate(vgk, vpk, model);
}

static bool solveLinear3x3Direct(float a00,
                                 float a01,
                                 float a02,
                                 float a10,
                                 float a11,
                                 float a12,
                                 float a20,
                                 float a21,
                                 float a22,
                                 const float rhs[3],
                                 float x[3]) {
  float c00 = a11 * a22 - a12 * a21;
  float c01 = a12 * a20 - a10 * a22;
  float c02 = a10 * a21 - a11 * a20;
  float det = a00 * c00 + a01 * c01 + a02 * c02;
  if (!std::isfinite(det) || std::fabs(det) < 1e-12f) {
    return false;
  }
  float invDet = 1.0f / det;

  x[0] = (rhs[0] * c00 +
          rhs[1] * (a02 * a21 - a01 * a22) +
          rhs[2] * (a01 * a12 - a02 * a11)) *
         invDet;
  x[1] = (rhs[0] * c01 +
          rhs[1] * (a00 * a22 - a02 * a20) +
          rhs[2] * (a02 * a10 - a00 * a12)) *
         invDet;
  x[2] = (rhs[0] * c02 +
          rhs[1] * (a01 * a20 - a00 * a21) +
          rhs[2] * (a00 * a11 - a01 * a10)) *
         invDet;
  return std::isfinite(x[0]) && std::isfinite(x[1]) && std::isfinite(x[2]);
}

static void configureRuntimeTriodeLut(KorenTriodeLut& lut,
                                      const KorenTriodeModel& model,
                                      float biasVolts,
                                      float plateSupplyVolts,
                                      float extraNegativeGridHeadroomVolts,
                                      float positiveGridHeadroomVolts) {
  float vgkMin = std::max(-140.0f, biasVolts - extraNegativeGridHeadroomVolts);
  float vgkMax = std::max(10.0f, positiveGridHeadroomVolts);
  float vpkMax =
      std::max(plateSupplyVolts + 80.0f, 1.25f * plateSupplyVolts);
  configureKorenTriodeLut(lut, model, vgkMin, vgkMax, 257, 0.0f, vpkMax, 385);
}

static inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

static inline float seededSignedUnit(uint32_t seed, uint32_t salt) {
  uint32_t h = hash32(seed ^ salt);
  return 2.0f * (static_cast<float>(h) * kRadioHashUnitInv) - 1.0f;
}

static inline float applySeededDrift(float base,
                                     float relativeDepth,
                                     uint32_t seed,
                                     uint32_t salt) {
  return base * (1.0f + relativeDepth * seededSignedUnit(seed, salt));
}

template <typename Nonlinear>
static inline float processOversampled2x(float x,
                                         float& prev,
                                         Biquad& lp1,
                                         Biquad& lp2,
                                         Nonlinear&& nonlinear) {
  float mid = 0.5f * (prev + x);
  float y0 = nonlinear(mid);
  float y1 = nonlinear(x);
  y0 = lp1.process(y0);
  y0 = lp2.process(y0);
  y1 = lp1.process(y1);
  y1 = lp2.process(y1);
  prev = x;
  return y1;
}

float Biquad::process(float x) {
  float y = b0 * x + z1;
  z1 = b1 * x - a1 * y + z2;
  z2 = b2 * x - a2 * y;
  return y;
}

void Biquad::reset() {
  z1 = 0.0f;
  z2 = 0.0f;
}

void Biquad::setLowpass(float sampleRate, float freq, float q) {
  float w0 = kRadioTwoPi * freq / sampleRate;
  float cosw = std::cos(w0);
  float sinw = std::sin(w0);
  float alpha = sinw / (2.0f * q);
  float a0 = 1.0f + alpha;
  b0 = (1.0f - cosw) / 2.0f / a0;
  b1 = (1.0f - cosw) / a0;
  b2 = (1.0f - cosw) / 2.0f / a0;
  a1 = -2.0f * cosw / a0;
  a2 = (1.0f - alpha) / a0;
}

void Biquad::setHighpass(float sampleRate, float freq, float q) {
  float w0 = kRadioTwoPi * freq / sampleRate;
  float cosw = std::cos(w0);
  float sinw = std::sin(w0);
  float alpha = sinw / (2.0f * q);
  float a0 = 1.0f + alpha;
  b0 = (1.0f + cosw) / 2.0f / a0;
  b1 = -(1.0f + cosw) / a0;
  b2 = (1.0f + cosw) / 2.0f / a0;
  a1 = -2.0f * cosw / a0;
  a2 = (1.0f - alpha) / a0;
}

void Biquad::setBandpass(float sampleRate, float freq, float q) {
  float w0 = kRadioTwoPi * freq / sampleRate;
  float cosw = std::cos(w0);
  float sinw = std::sin(w0);
  float alpha = sinw / (2.0f * q);
  float a0 = 1.0f + alpha;
  b0 = alpha / a0;
  b1 = 0.0f;
  b2 = -alpha / a0;
  a1 = -2.0f * cosw / a0;
  a2 = (1.0f - alpha) / a0;
}

void Biquad::setPeaking(float sampleRate, float freq, float q, float gainDb) {
  float a = std::pow(10.0f, gainDb / 40.0f);
  float w0 = kRadioTwoPi * freq / sampleRate;
  float cosw = std::cos(w0);
  float sinw = std::sin(w0);
  float alpha = sinw / (2.0f * q);
  float a0 = 1.0f + alpha / a;
  b0 = (1.0f + alpha * a) / a0;
  b1 = -2.0f * cosw / a0;
  b2 = (1.0f - alpha * a) / a0;
  a1 = -2.0f * cosw / a0;
  a2 = (1.0f - alpha / a) / a0;
}

void SeriesRlcBandpass::configure(float newFs,
                                  float newInductanceHenries,
                                  float newCapacitanceFarads,
                                  float newSeriesResistanceOhms,
                                  float newOutputResistanceOhms,
                                  int newIntegrationSubsteps) {
  assert(std::isfinite(newFs) && newFs > 0.0f);
  assert(std::isfinite(newInductanceHenries) && newInductanceHenries > 0.0f);
  assert(std::isfinite(newCapacitanceFarads) && newCapacitanceFarads > 0.0f);
  assert(std::isfinite(newSeriesResistanceOhms) && newSeriesResistanceOhms >= 0.0f);
  assert(std::isfinite(newOutputResistanceOhms) && newOutputResistanceOhms >= 0.0f);
  assert(newIntegrationSubsteps > 0);
  fs = newFs;
  inductanceHenries = newInductanceHenries;
  capacitanceFarads = newCapacitanceFarads;
  seriesResistanceOhms = newSeriesResistanceOhms;
  outputResistanceOhms = newOutputResistanceOhms;
  integrationSubsteps = newIntegrationSubsteps;
  dtSub = 1.0f / (fs * static_cast<float>(integrationSubsteps));
  float subA00 =
      1.0f - dtSub * (seriesResistanceOhms / inductanceHenries);
  float subA01 = -dtSub / inductanceHenries;
  float subB0 = dtSub / inductanceHenries;
  float capStep = dtSub / capacitanceFarads;
  float subA10 = capStep * subA00;
  float subA11 = 1.0f + capStep * subA01;
  float subB1 = capStep * subB0;
  macroA00 = 1.0f;
  macroA01 = 0.0f;
  macroA10 = 0.0f;
  macroA11 = 1.0f;
  macroB0 = 0.0f;
  macroB1 = 0.0f;
  for (int step = 0; step < integrationSubsteps; ++step) {
    float nextA00 = subA00 * macroA00 + subA01 * macroA10;
    float nextA01 = subA00 * macroA01 + subA01 * macroA11;
    float nextA10 = subA10 * macroA00 + subA11 * macroA10;
    float nextA11 = subA10 * macroA01 + subA11 * macroA11;
    float nextB0 = subA00 * macroB0 + subA01 * macroB1 + subB0;
    float nextB1 = subA10 * macroB0 + subA11 * macroB1 + subB1;
    macroA00 = nextA00;
    macroA01 = nextA01;
    macroA10 = nextA10;
    macroA11 = nextA11;
    macroB0 = nextB0;
    macroB1 = nextB1;
  }
  // Retuning a live LC network changes component values, but it does not
  // instantaneously zero inductor current or capacitor voltage. Runtime resets
  // are handled by the owning stage when the radio itself is reset.
}

void SeriesRlcBandpass::reset() {
  inductorCurrent = 0.0f;
  capacitorVoltage = 0.0f;
}

float SeriesRlcBandpass::process(float vin) {
  float nextInductorCurrent =
      macroA00 * inductorCurrent + macroA01 * capacitorVoltage + macroB0 * vin;
  float nextCapacitorVoltage =
      macroA10 * inductorCurrent + macroA11 * capacitorVoltage + macroB1 * vin;
  inductorCurrent = nextInductorCurrent;
  capacitorVoltage = nextCapacitorVoltage;
  assert(std::isfinite(inductorCurrent) && std::isfinite(capacitorVoltage));
  return inductorCurrent * outputResistanceOhms;
}

void CoupledTunedTransformer::configure(float newFs,
                                        float newPrimaryInductanceHenries,
                                        float newPrimaryCapacitanceFarads,
                                        float newPrimaryResistanceOhms,
                                        float newSecondaryInductanceHenries,
                                        float newSecondaryCapacitanceFarads,
                                        float newSecondaryResistanceOhms,
                                        float newCouplingCoeff,
                                        float newOutputResistanceOhms,
                                        int newIntegrationSubsteps) {
  assert(std::isfinite(newFs) && newFs > 0.0f);
  assert(std::isfinite(newPrimaryInductanceHenries) &&
         newPrimaryInductanceHenries > 0.0f);
  assert(std::isfinite(newPrimaryCapacitanceFarads) &&
         newPrimaryCapacitanceFarads > 0.0f);
  assert(std::isfinite(newPrimaryResistanceOhms) && newPrimaryResistanceOhms >= 0.0f);
  assert(std::isfinite(newSecondaryInductanceHenries) &&
         newSecondaryInductanceHenries > 0.0f);
  assert(std::isfinite(newSecondaryCapacitanceFarads) &&
         newSecondaryCapacitanceFarads > 0.0f);
  assert(std::isfinite(newSecondaryResistanceOhms) &&
         newSecondaryResistanceOhms >= 0.0f);
  assert(std::isfinite(newCouplingCoeff) && std::fabs(newCouplingCoeff) < 1.0f);
  assert(std::isfinite(newOutputResistanceOhms) && newOutputResistanceOhms >= 0.0f);
  assert(newIntegrationSubsteps > 0);
  fs = newFs;
  primaryInductanceHenries = newPrimaryInductanceHenries;
  primaryCapacitanceFarads = newPrimaryCapacitanceFarads;
  primaryResistanceOhms = newPrimaryResistanceOhms;
  secondaryInductanceHenries = newSecondaryInductanceHenries;
  secondaryCapacitanceFarads = newSecondaryCapacitanceFarads;
  secondaryResistanceOhms = newSecondaryResistanceOhms;
  couplingCoeff = newCouplingCoeff;
  outputResistanceOhms = newOutputResistanceOhms;
  integrationSubsteps = newIntegrationSubsteps;
  mutualInductance =
      couplingCoeff *
      std::sqrt(primaryInductanceHenries * secondaryInductanceHenries);
  float determinant = primaryInductanceHenries * secondaryInductanceHenries -
                      mutualInductance * mutualInductance;
  assert(determinant > 0.0f);
  determinantInv = 1.0f / determinant;
  dtSub = 1.0f / (fs * static_cast<float>(integrationSubsteps));
  float k = dtSub * determinantInv;
  float subA00 =
      1.0f - k * secondaryInductanceHenries * primaryResistanceOhms;
  float subA01 = k * mutualInductance * secondaryResistanceOhms;
  float subA02 = -k * secondaryInductanceHenries;
  float subA03 = k * mutualInductance;
  float subA10 = k * mutualInductance * primaryResistanceOhms;
  float subA11 =
      1.0f - k * primaryInductanceHenries * secondaryResistanceOhms;
  float subA12 = k * mutualInductance;
  float subA13 = -k * primaryInductanceHenries;
  float subB0 = k * secondaryInductanceHenries;
  float subB1 = -k * mutualInductance;
  float primaryCapStep = dtSub / primaryCapacitanceFarads;
  float secondaryCapStep = dtSub / secondaryCapacitanceFarads;
  std::array<float, 16> subA{
      subA00,
      subA01,
      subA02,
      subA03,
      subA10,
      subA11,
      subA12,
      subA13,
      primaryCapStep * subA00,
      primaryCapStep * subA01,
      1.0f + primaryCapStep * subA02,
      primaryCapStep * subA03,
      secondaryCapStep * subA10,
      secondaryCapStep * subA11,
      secondaryCapStep * subA12,
      1.0f + secondaryCapStep * subA13,
  };
  std::array<float, 4> subB{
      subB0,
      subB1,
      primaryCapStep * subB0,
      secondaryCapStep * subB1,
  };
  macroA = {};
  macroA[0] = 1.0f;
  macroA[5] = 1.0f;
  macroA[10] = 1.0f;
  macroA[15] = 1.0f;
  macroB = {};
  for (int step = 0; step < integrationSubsteps; ++step) {
    std::array<float, 16> nextA{};
    std::array<float, 4> nextB{};
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        float sum = 0.0f;
        for (int kIdx = 0; kIdx < 4; ++kIdx) {
          sum += subA[row * 4 + kIdx] * macroA[kIdx * 4 + col];
        }
        nextA[row * 4 + col] = sum;
      }
      float sum = subB[row];
      for (int kIdx = 0; kIdx < 4; ++kIdx) {
        sum += subA[row * 4 + kIdx] * macroB[kIdx];
      }
      nextB[row] = sum;
    }
    macroA = nextA;
    macroB = nextB;
  }
  // Preserve stored magnetic/electric state across retunes. A tuned IF can
  // move its resonance while currents and capacitor voltages remain continuous;
  // hard zeroing here causes audible zippering/stutter during AFC/tuning.
}

void CoupledTunedTransformer::reset() {
  primaryCurrent = 0.0f;
  primaryCapVoltage = 0.0f;
  secondaryCurrent = 0.0f;
  secondaryCapVoltage = 0.0f;
}

float CoupledTunedTransformer::process(float vin) {
  float nextPrimaryCurrent = macroA[0] * primaryCurrent +
                             macroA[1] * secondaryCurrent +
                             macroA[2] * primaryCapVoltage +
                             macroA[3] * secondaryCapVoltage + macroB[0] * vin;
  float nextSecondaryCurrent = macroA[4] * primaryCurrent +
                               macroA[5] * secondaryCurrent +
                               macroA[6] * primaryCapVoltage +
                               macroA[7] * secondaryCapVoltage + macroB[1] * vin;
  float nextPrimaryCapVoltage = macroA[8] * primaryCurrent +
                                macroA[9] * secondaryCurrent +
                                macroA[10] * primaryCapVoltage +
                                macroA[11] * secondaryCapVoltage + macroB[2] * vin;
  float nextSecondaryCapVoltage = macroA[12] * primaryCurrent +
                                  macroA[13] * secondaryCurrent +
                                  macroA[14] * primaryCapVoltage +
                                  macroA[15] * secondaryCapVoltage + macroB[3] * vin;
  primaryCurrent = nextPrimaryCurrent;
  secondaryCurrent = nextSecondaryCurrent;
  primaryCapVoltage = nextPrimaryCapVoltage;
  secondaryCapVoltage = nextSecondaryCapVoltage;
  assert(std::isfinite(primaryCurrent) && std::isfinite(primaryCapVoltage) &&
         std::isfinite(secondaryCurrent) &&
         std::isfinite(secondaryCapVoltage));

  return secondaryCurrent * outputResistanceOhms;
}

static CurrentDrivenTransformerSample projectNoShuntCap(
    const CurrentDrivenTransformer& transformer,
    float primaryDriveCurrentAmps,
    float secondaryLoadResistanceOhms,
    float primaryLoadResistanceOhms) {
  float dt = requirePositiveFinite(transformer.dtSub);
  float Rp = transformer.primaryResistanceOhms;
  float Rs = transformer.secondaryResistanceOhms;
  float Lp = transformer.cachedPrimaryInductance;
  float Ls = transformer.cachedSecondaryInductance;
  float M = transformer.cachedMutualInductance;
  float coreLossConductance =
      (transformer.primaryCoreLossResistanceOhms > 0.0f)
          ? 1.0f / transformer.primaryCoreLossResistanceOhms
          : 0.0f;
  float primaryLoadConductance =
      (primaryLoadResistanceOhms > 0.0f &&
       std::isfinite(primaryLoadResistanceOhms))
          ? 1.0f / primaryLoadResistanceOhms
          : 0.0f;
  float Gp = coreLossConductance + primaryLoadConductance;
  float Gs = (secondaryLoadResistanceOhms > 0.0f &&
              std::isfinite(secondaryLoadResistanceOhms))
                 ? 1.0f / secondaryLoadResistanceOhms
                 : 0.0f;
  float projectedPrimaryCurrent = transformer.primaryCurrent;
  float projectedSecondaryCurrent = transformer.secondaryCurrent;
  float projectedPrimaryVoltage = transformer.primaryVoltage;
  float projectedSecondaryVoltage = transformer.secondaryVoltage;
  float lpOverDt = Lp / dt;
  float lsOverDt = Ls / dt;
  float mOverDt = M / dt;
  float a11 = Rp + lpOverDt;
  float a12 = mOverDt;
  float a21 = mOverDt;
  float a22 = Rs + lsOverDt;
  bool primaryConductive = Gp > 0.0f;
  bool secondaryConductive = Gs > 0.0f;
  float invGp = primaryConductive ? 1.0f / Gp : 0.0f;
  float invGs = secondaryConductive ? 1.0f / Gs : 0.0f;

  for (int step = 0; step < transformer.integrationSubsteps; ++step) {
    float ipPrev = projectedPrimaryCurrent;
    float isPrev = projectedSecondaryCurrent;
    float c1 = lpOverDt * ipPrev + mOverDt * isPrev;
    float c2 = mOverDt * ipPrev + lsOverDt * isPrev;

    if (!primaryConductive) {
      projectedPrimaryCurrent = primaryDriveCurrentAmps;
      if (!secondaryConductive) {
        projectedSecondaryCurrent = 0.0f;
      } else {
        float denom = a22 + invGs;
        assert(std::fabs(denom) >= 1e-12f);
        projectedSecondaryCurrent =
            (c2 - a21 * projectedPrimaryCurrent) / denom;
      }
    } else if (!secondaryConductive) {
      projectedSecondaryCurrent = 0.0f;
      float denom = a11 + invGp;
      assert(std::fabs(denom) >= 1e-12f);
      projectedPrimaryCurrent =
          (c1 + primaryDriveCurrentAmps * invGp) / denom;
    } else {
      float det = (a11 + invGp) * (a22 + invGs) - a12 * a21;
      assert(std::fabs(det) >= 1e-12f);
      float rhs0 = c1 + primaryDriveCurrentAmps * invGp;
      float rhs1 = c2;
      projectedPrimaryCurrent =
          (rhs0 * (a22 + invGs) - a12 * rhs1) / det;
      projectedSecondaryCurrent =
          ((a11 + invGp) * rhs1 - rhs0 * a21) / det;
    }

    projectedPrimaryVoltage = primaryConductive
                                  ? ((primaryDriveCurrentAmps -
                                      projectedPrimaryCurrent) *
                                     invGp)
                                  : (a11 * projectedPrimaryCurrent +
                                     a12 * projectedSecondaryCurrent - c1);
    projectedSecondaryVoltage = secondaryConductive
                                    ? (-projectedSecondaryCurrent * invGs)
                                    : (a21 * projectedPrimaryCurrent +
                                       a22 * projectedSecondaryCurrent - c2);
    assert(std::isfinite(projectedPrimaryCurrent) &&
           std::isfinite(projectedSecondaryCurrent) &&
           std::isfinite(projectedPrimaryVoltage) &&
           std::isfinite(projectedSecondaryVoltage));
  }

  return CurrentDrivenTransformerSample{
      projectedPrimaryVoltage, projectedSecondaryVoltage,
      projectedPrimaryCurrent, projectedSecondaryCurrent};
}

struct AffineTransformerProjection {
  CurrentDrivenTransformerSample base{};
  CurrentDrivenTransformerSample slope{};
};

struct FixedLoadAffineTransformerProjection {
  std::array<float, 16> stateA{};
  CurrentDrivenTransformerSample slope{};
};

static float transformerStateComponent(const CurrentDrivenTransformerSample& s,
                                       int index) {
  switch (index) {
    case 0:
      return s.primaryCurrent;
    case 1:
      return s.secondaryCurrent;
    case 2:
      return s.primaryVoltage;
    default:
      return s.secondaryVoltage;
  }
}

static CurrentDrivenTransformerSample evalFixedLoadAffineBase(
    const std::array<float, 16>& stateA,
    const CurrentDrivenTransformer& t) {
  float state[4] = {t.primaryCurrent, t.secondaryCurrent, t.primaryVoltage,
                    t.secondaryVoltage};
  float next[4] = {};
  for (int row = 0; row < 4; ++row) {
    float sum = 0.0f;
    for (int col = 0; col < 4; ++col) {
      sum += stateA[row * 4 + col] * state[col];
    }
    next[row] = sum;
  }

  return CurrentDrivenTransformerSample{
      next[2], next[3], next[0], next[1]};
}

static FixedLoadAffineTransformerProjection buildFixedLoadAffineProjection(
    const CurrentDrivenTransformer& t,
    float secondaryLoadResistanceOhms,
    float primaryLoadResistanceOhms) {
  FixedLoadAffineTransformerProjection p{};

  CurrentDrivenTransformer zero = t;
  zero.reset();
  p.slope =
      zero.project(1.0f, secondaryLoadResistanceOhms, primaryLoadResistanceOhms);

  for (int col = 0; col < 4; ++col) {
    CurrentDrivenTransformer basis = t;
    basis.reset();
    switch (col) {
      case 0:
        basis.primaryCurrent = 1.0f;
        break;
      case 1:
        basis.secondaryCurrent = 1.0f;
        break;
      case 2:
        basis.primaryVoltage = 1.0f;
        break;
      case 3:
        basis.secondaryVoltage = 1.0f;
        break;
    }
    CurrentDrivenTransformerSample out =
        basis.project(0.0f, secondaryLoadResistanceOhms, primaryLoadResistanceOhms);
    for (int row = 0; row < 4; ++row) {
      p.stateA[row * 4 + col] = transformerStateComponent(out, row);
    }
  }

  return p;
}

static AffineTransformerProjection buildAffineProjection(
    const CurrentDrivenTransformer& t,
    float secondaryLoadResistanceOhms,
    float primaryLoadResistanceOhms) {
  auto s0 =
      t.project(0.0f, secondaryLoadResistanceOhms, primaryLoadResistanceOhms);
  auto s1 =
      t.project(1.0f, secondaryLoadResistanceOhms, primaryLoadResistanceOhms);

  AffineTransformerProjection p{};
  p.base = s0;
  p.slope.primaryVoltage = s1.primaryVoltage - s0.primaryVoltage;
  p.slope.secondaryVoltage = s1.secondaryVoltage - s0.secondaryVoltage;
  p.slope.primaryCurrent = s1.primaryCurrent - s0.primaryCurrent;
  p.slope.secondaryCurrent = s1.secondaryCurrent - s0.secondaryCurrent;
  return p;
}

static CurrentDrivenTransformerSample evalAffineProjection(
    const AffineTransformerProjection& p,
    float driveCurrent) {
  CurrentDrivenTransformerSample s{};
  s.primaryVoltage = p.base.primaryVoltage + driveCurrent * p.slope.primaryVoltage;
  s.secondaryVoltage =
      p.base.secondaryVoltage + driveCurrent * p.slope.secondaryVoltage;
  s.primaryCurrent = p.base.primaryCurrent + driveCurrent * p.slope.primaryCurrent;
  s.secondaryCurrent =
      p.base.secondaryCurrent + driveCurrent * p.slope.secondaryCurrent;
  return s;
}

static float solveOutputPrimaryVoltageAffine(
    const AffineTransformerProjection& proj,
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float gridA,
    float gridB,
    float initialVp) {
  float vp = initialVp;

  for (int iter = 0; iter < 8; ++iter) {
    float plateA = outputPlateQuiescent - 0.5f * vp;
    float plateB = outputPlateQuiescent + 0.5f * vp;
    KorenTriodePlateEval evalA = evaluateKorenTriodePlateRuntime(
        power.outputTubeBiasVolts + gridA, plateA, power.outputTubeTriodeModel,
        power.outputTubeTriodeLut);
    KorenTriodePlateEval evalB = evaluateKorenTriodePlateRuntime(
        power.outputTubeBiasVolts + gridB, plateB, power.outputTubeTriodeModel,
        power.outputTubeTriodeLut);

    float drive =
        0.5f * static_cast<float>(evalA.currentAmps - evalB.currentAmps);
    float driveSlope = -0.25f * static_cast<float>(
                                   evalA.conductanceSiemens +
                                   evalB.conductanceSiemens);

    float f =
        proj.base.primaryVoltage + proj.slope.primaryVoltage * drive - vp;
    float df = proj.slope.primaryVoltage * driveSlope - 1.0f;
    assert(std::isfinite(df) && std::fabs(df) >= 1e-9f);

    vp -= f / df;
    assert(std::isfinite(vp));
  }

  return vp;
}

static float estimateOutputStageNominalPowerWatts(
    const Radio1938::PowerNodeState& power) {
  float loadResistance = requirePositiveFinite(power.outputLoadResistanceOhms);
  float outputPlateQuiescent =
      requirePositiveFinite(power.outputTubeQuiescentPlateVolts);
  float maxGridPeak = 0.92f * std::max(std::fabs(power.outputTubeBiasVolts), 1.0f);
  constexpr int kAmplitudeSteps = 40;
  constexpr int kSamplesPerCycle = 96;
  constexpr int kSettleCycles = 6;
  constexpr int kMeasureCycles = 2;
  float bestSecondaryRms = 0.0f;

  for (int step = 1; step <= kAmplitudeSteps; ++step) {
    float gridPeak =
        maxGridPeak * static_cast<float>(step) / static_cast<float>(kAmplitudeSteps);
    CurrentDrivenTransformer transformer = power.outputTransformer;
    transformer.reset();
    double sumSq = 0.0;
    int sampleCount = 0;

    for (int cycle = 0; cycle < (kSettleCycles + kMeasureCycles); ++cycle) {
      for (int i = 0; i < kSamplesPerCycle; ++i) {
        float phase = kRadioTwoPi * static_cast<float>(i) /
                      static_cast<float>(kSamplesPerCycle);
        float gridA = gridPeak * std::sin(phase);
        float gridB = -gridA;
        AffineTransformerProjection affineOut =
            buildAffineProjection(transformer, loadResistance, 0.0f);
        float solvedPrimaryVoltage = solveOutputPrimaryVoltageAffine(
            affineOut, power, outputPlateQuiescent, gridA, gridB,
            transformer.primaryVoltage);
        float plateA = outputPlateQuiescent - 0.5f * solvedPrimaryVoltage;
        float plateB = outputPlateQuiescent + 0.5f * solvedPrimaryVoltage;
        KorenTriodePlateEval evalA = evaluateKorenTriodePlateRuntime(
            power.outputTubeBiasVolts + gridA, plateA,
            power.outputTubeTriodeModel, power.outputTubeTriodeLut);
        KorenTriodePlateEval evalB = evaluateKorenTriodePlateRuntime(
            power.outputTubeBiasVolts + gridB, plateB,
            power.outputTubeTriodeModel, power.outputTubeTriodeLut);
        float driveCurrent = 0.5f * static_cast<float>(evalA.currentAmps -
                                                       evalB.currentAmps);
        auto outputSample = transformer.process(driveCurrent, loadResistance, 0.0f);
        if (cycle >= kSettleCycles) {
          sumSq += static_cast<double>(outputSample.secondaryVoltage) *
                   static_cast<double>(outputSample.secondaryVoltage);
          sampleCount++;
        }
      }
    }

    float secondaryRms = std::sqrt(sumSq / std::max(sampleCount, 1));
    bestSecondaryRms = std::max(bestSecondaryRms, secondaryRms);
  }

  return (bestSecondaryRms * bestSecondaryRms) / loadResistance;
}

struct DriverInterstageCenterTappedResult {
  float driverPlateCurrentAbs = 0.0f;
  float primaryCurrent = 0.0f;
  float primaryVoltage = 0.0f;
  float secondaryACurrent = 0.0f;
  float secondaryAVoltage = 0.0f;
  float secondaryBCurrent = 0.0f;
  float secondaryBVoltage = 0.0f;
};

static DriverInterstageCenterTappedResult solveDriverInterstageCenterTappedNoCap(
    const CurrentDrivenTransformer& t,
    const Radio1938::PowerNodeState& power,
    float controlGridVolts,
    float driverPlateQuiescent,
    float driverQuiescentCurrent) {
  assert(power.tubeTriodeConnected);

  float dt = requirePositiveFinite(t.dtSub);
  float nTotal = requirePositiveFinite(t.cachedTurns);
  float nHalf = 2.0f * nTotal;

  float Rp = t.primaryResistanceOhms;
  float Ra = 0.5f * t.secondaryResistanceOhms;
  float Rb = 0.5f * t.secondaryResistanceOhms;

  float Lp = t.cachedPrimaryInductance;
  float LlkHalf = 0.5f * t.secondaryLeakageInductanceHenries;
  float LhalfMag = t.magnetizingInductanceHenries / (nHalf * nHalf);
  float La = LlkHalf + LhalfMag;
  float Lb = LlkHalf + LhalfMag;
  float Mab = LhalfMag;
  float M = 0.5f * t.cachedMutualInductance;

  float coreLossConductance =
      (t.primaryCoreLossResistanceOhms > 0.0f)
          ? (1.0f / t.primaryCoreLossResistanceOhms)
          : 0.0f;

  float vp = power.interstageCt.primaryVoltage;
  float va = power.interstageCt.secondaryAVoltage;
  float vb = power.interstageCt.secondaryBVoltage;
  float ip = power.interstageCt.primaryCurrent;
  float ia = power.interstageCt.secondaryACurrent;
  float ib = power.interstageCt.secondaryBCurrent;
  float lpOverDt = Lp / dt;
  float laOverDt = La / dt;
  float lbOverDt = Lb / dt;
  float mOverDt = M / dt;
  float mabOverDt = Mab / dt;

  for (int step = 0; step < t.integrationSubsteps; ++step) {
    float ipPrev = ip;
    float iaPrev = ia;
    float ibPrev = ib;
    float cPrimary = lpOverDt * ipPrev + mOverDt * iaPrev - mOverDt * ibPrev;
    float cSecondaryA = mOverDt * ipPrev + laOverDt * iaPrev - mabOverDt * ibPrev;
    float cSecondaryB =
        -mOverDt * ipPrev - mabOverDt * iaPrev + lbOverDt * ibPrev;

    for (int iter = 0; iter < 12; ++iter) {
      float driverPlateVolts = driverPlateQuiescent - vp;
      KorenTriodePlateEval driverEval = evaluateKorenTriodePlateRuntime(
          controlGridVolts, driverPlateVolts, power.tubeTriodeModel,
          power.tubeTriodeLut);
      float driverPlateCurrentAbs = static_cast<float>(driverEval.currentAmps);
      float dIdrive_dVp =
          -static_cast<float>(driverEval.conductanceSiemens);
      float idrive = driverPlateCurrentAbs - driverQuiescentCurrent;
      float ipNow = idrive - coreLossConductance * vp;

      float iaBranch = tubeGridBranchCurrent(
          va, power.outputTubeBiasVolts, power.outputGridLeakResistanceOhms,
          power.outputGridCurrentResistanceOhms);
      float iaNow = -iaBranch;
      float dIa_dVa = -tubeGridBranchSlope(
          va, power.outputTubeBiasVolts, power.outputGridLeakResistanceOhms,
          power.outputGridCurrentResistanceOhms);
      float ibBranch = tubeGridBranchCurrent(
          vb, power.outputTubeBiasVolts, power.outputGridLeakResistanceOhms,
          power.outputGridCurrentResistanceOhms);
      float ibNow = -ibBranch;
      float dIb_dVb = -tubeGridBranchSlope(
          vb, power.outputTubeBiasVolts, power.outputGridLeakResistanceOhms,
          power.outputGridCurrentResistanceOhms);
      float dIp_dVp = dIdrive_dVp - coreLossConductance;

      float f[3] = {
          (Rp + lpOverDt) * ipNow + mOverDt * iaNow - mOverDt * ibNow - vp -
              cPrimary,
          mOverDt * ipNow + (Ra + laOverDt) * iaNow - mabOverDt * ibNow - va -
              cSecondaryA,
          -mOverDt * ipNow - mabOverDt * iaNow + (Rb + lbOverDt) * ibNow - vb -
              cSecondaryB,
      };

      float j[3][3] = {
          {(Rp + lpOverDt) * dIp_dVp - 1.0f, mOverDt * dIa_dVa,
           -mOverDt * dIb_dVb},
          {mOverDt * dIp_dVp, (Ra + laOverDt) * dIa_dVa - 1.0f,
           -mabOverDt * dIb_dVb},
          {-mOverDt * dIp_dVp, -mabOverDt * dIa_dVa,
           (Rb + lbOverDt) * dIb_dVb - 1.0f},
      };

      float rhs[3] = {-f[0], -f[1], -f[2]};
      float delta[3] = {};
      if (!solveLinear3x3Direct(j[0][0], j[0][1], j[0][2], j[1][0], j[1][1],
                                j[1][2], j[2][0], j[2][1], j[2][2], rhs,
                                delta)) {
        assert(false && "interstage 3x3 solve failed");
      }

      vp += delta[0];
      va += delta[1];
      vb += delta[2];

      float maxDelta = std::max(
          std::fabs(delta[0]),
          std::max(std::fabs(delta[1]), std::fabs(delta[2])));

      if (maxDelta < 1e-6f) {
        break;
      }

      assert(std::isfinite(vp));
      assert(std::isfinite(va));
      assert(std::isfinite(vb));
    }

    float driverPlateVolts = driverPlateQuiescent - vp;
    float driverPlateCurrentAbs = static_cast<float>(
        evaluateKorenTriodePlateRuntime(controlGridVolts, driverPlateVolts,
                                        power.tubeTriodeModel,
                                        power.tubeTriodeLut)
            .currentAmps);
    ip = driverPlateCurrentAbs - driverQuiescentCurrent -
         coreLossConductance * vp;
    ia = -tubeGridBranchCurrent(
        va, power.outputTubeBiasVolts, power.outputGridLeakResistanceOhms,
        power.outputGridCurrentResistanceOhms);
    ib = -tubeGridBranchCurrent(
        vb, power.outputTubeBiasVolts, power.outputGridLeakResistanceOhms,
        power.outputGridCurrentResistanceOhms);
  }

  float finalDriverPlateVolts = driverPlateQuiescent - vp;
  float finalDriverPlateCurrentAbs = static_cast<float>(
      evaluateKorenTriodePlateRuntime(controlGridVolts, finalDriverPlateVolts,
                                      power.tubeTriodeModel,
                                      power.tubeTriodeLut)
          .currentAmps);

  DriverInterstageCenterTappedResult out{};
  out.driverPlateCurrentAbs = finalDriverPlateCurrentAbs;
  out.primaryCurrent = ip;
  out.primaryVoltage = vp;
  out.secondaryACurrent = ia;
  out.secondaryAVoltage = va;
  out.secondaryBCurrent = ib;
  out.secondaryBVoltage = vb;
  return out;
}

void CurrentDrivenTransformer::configure(
    float newFs,
    float newPrimaryLeakageInductanceHenries,
    float newMagnetizingInductanceHenries,
    float newTurnsRatioPrimaryToSecondary,
    float newPrimaryResistanceOhms,
    float newPrimaryCoreLossResistanceOhms,
    float newPrimaryShuntCapFarads,
    float newSecondaryLeakageInductanceHenries,
    float newSecondaryResistanceOhms,
    float newSecondaryShuntCapFarads,
    int newIntegrationSubsteps) {
  assert(std::isfinite(newFs) && newFs > 0.0f);
  assert(std::isfinite(newPrimaryLeakageInductanceHenries) &&
         newPrimaryLeakageInductanceHenries >= 0.0f);
  assert(std::isfinite(newMagnetizingInductanceHenries) &&
         newMagnetizingInductanceHenries > 0.0f);
  assert(std::isfinite(newTurnsRatioPrimaryToSecondary) &&
         newTurnsRatioPrimaryToSecondary > 0.0f);
  assert(std::isfinite(newPrimaryResistanceOhms) && newPrimaryResistanceOhms >= 0.0f);
  assert(std::isfinite(newPrimaryCoreLossResistanceOhms) &&
         newPrimaryCoreLossResistanceOhms >= 0.0f);
  assert(std::isfinite(newPrimaryShuntCapFarads) && newPrimaryShuntCapFarads >= 0.0f);
  assert(std::isfinite(newSecondaryLeakageInductanceHenries) &&
         newSecondaryLeakageInductanceHenries >= 0.0f);
  assert(std::isfinite(newSecondaryResistanceOhms) &&
         newSecondaryResistanceOhms >= 0.0f);
  assert(std::isfinite(newSecondaryShuntCapFarads) &&
         newSecondaryShuntCapFarads >= 0.0f);
  assert(newIntegrationSubsteps > 0);
  fs = newFs;
  primaryLeakageInductanceHenries = newPrimaryLeakageInductanceHenries;
  magnetizingInductanceHenries = newMagnetizingInductanceHenries;
  turnsRatioPrimaryToSecondary = newTurnsRatioPrimaryToSecondary;
  primaryResistanceOhms = newPrimaryResistanceOhms;
  primaryCoreLossResistanceOhms = newPrimaryCoreLossResistanceOhms;
  primaryShuntCapFarads = newPrimaryShuntCapFarads;
  secondaryLeakageInductanceHenries = newSecondaryLeakageInductanceHenries;
  secondaryResistanceOhms = newSecondaryResistanceOhms;
  secondaryShuntCapFarads = newSecondaryShuntCapFarads;
  integrationSubsteps = newIntegrationSubsteps;
  dtSub = 1.0f / (fs * static_cast<float>(integrationSubsteps));
  cachedTurns = turnsRatioPrimaryToSecondary;
  cachedPrimaryInductance =
      primaryLeakageInductanceHenries + magnetizingInductanceHenries;
  cachedSecondaryInductance =
      secondaryLeakageInductanceHenries +
      magnetizingInductanceHenries / (cachedTurns * cachedTurns);
  cachedMutualInductance = magnetizingInductanceHenries / cachedTurns;
  reset();
}

void CurrentDrivenTransformer::reset() {
  primaryCurrent = 0.0f;
  secondaryCurrent = 0.0f;
  primaryVoltage = 0.0f;
  secondaryVoltage = 0.0f;
}

CurrentDrivenTransformerSample CurrentDrivenTransformer::project(
    float primaryDriveCurrentAmps,
    float secondaryLoadResistanceOhms,
    float primaryLoadResistanceOhms) const {
  if (primaryShuntCapFarads <= 0.0f && secondaryShuntCapFarads <= 0.0f) {
    return projectNoShuntCap(*this, primaryDriveCurrentAmps,
                             secondaryLoadResistanceOhms,
                             primaryLoadResistanceOhms);
  }

  float dt = requirePositiveFinite(dtSub);
  float primaryInductance = cachedPrimaryInductance;
  float secondaryInductance = cachedSecondaryInductance;
  float mutualInductance = cachedMutualInductance;
  float primaryCap = requirePositiveFinite(primaryShuntCapFarads);
  float secondaryCap = requirePositiveFinite(secondaryShuntCapFarads);
  float primaryCoreConductance =
      (primaryCoreLossResistanceOhms > 0.0f)
          ? 1.0f / primaryCoreLossResistanceOhms
          : 0.0f;
  if (primaryLoadResistanceOhms > 0.0f &&
      std::isfinite(primaryLoadResistanceOhms)) {
    primaryCoreConductance += 1.0f / primaryLoadResistanceOhms;
  }
  float secondaryLoadConductance =
      (secondaryLoadResistanceOhms > 0.0f &&
       std::isfinite(secondaryLoadResistanceOhms))
          ? 1.0f / secondaryLoadResistanceOhms
          : 0.0f;
  float determinant = primaryInductance * secondaryInductance -
                      mutualInductance * mutualInductance;
  assert(determinant > 0.0f);
  float a11 = secondaryInductance / determinant;
  float a12 = -mutualInductance / determinant;
  float a21 = -mutualInductance / determinant;
  float a22 = primaryInductance / determinant;
  float projectedPrimaryCurrent = primaryCurrent;
  float projectedSecondaryCurrent = secondaryCurrent;
  float projectedPrimaryVoltage = primaryVoltage;
  float projectedSecondaryVoltage = secondaryVoltage;
  constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

  for (int step = 0; step < integrationSubsteps; ++step) {
    float system[4][4] = {
        {1.0f + 0.5f * dt * a11 * primaryResistanceOhms,
         0.5f * dt * a12 * secondaryResistanceOhms,
         -0.5f * dt * a11, -0.5f * dt * a12},
        {0.5f * dt * a21 * primaryResistanceOhms,
         1.0f + 0.5f * dt * a22 * secondaryResistanceOhms,
         -0.5f * dt * a21, -0.5f * dt * a22},
        {0.5f * dt / primaryCap, 0.0f,
         1.0f + 0.5f * dt * primaryCoreConductance / primaryCap, 0.0f},
        {0.0f, 0.5f * dt / secondaryCap, 0.0f,
         1.0f + 0.5f * dt * secondaryLoadConductance / secondaryCap},
    };
    float b[4] = {
        (1.0f - 0.5f * dt * a11 * primaryResistanceOhms) *
                projectedPrimaryCurrent -
            0.5f * dt * a12 * secondaryResistanceOhms *
                projectedSecondaryCurrent +
            0.5f * dt * a11 * projectedPrimaryVoltage +
            0.5f * dt * a12 * projectedSecondaryVoltage,
        -0.5f * dt * a21 * primaryResistanceOhms * projectedPrimaryCurrent +
            (1.0f - 0.5f * dt * a22 * secondaryResistanceOhms) *
                projectedSecondaryCurrent +
            0.5f * dt * a21 * projectedPrimaryVoltage +
            0.5f * dt * a22 * projectedSecondaryVoltage,
        -0.5f * dt / primaryCap * projectedPrimaryCurrent +
            (1.0f - 0.5f * dt * primaryCoreConductance / primaryCap) *
                projectedPrimaryVoltage +
            dt * (primaryDriveCurrentAmps / primaryCap),
        -0.5f * dt / secondaryCap * projectedSecondaryCurrent +
            (1.0f - 0.5f * dt * secondaryLoadConductance / secondaryCap) *
                projectedSecondaryVoltage,
    };
    float x[4] = {kNaN, kNaN, kNaN, kNaN};
    bool solved = solveLinear4x4(system, b, x);
    assert(solved);
    projectedPrimaryCurrent = x[0];
    projectedSecondaryCurrent = x[1];
    projectedPrimaryVoltage = x[2];
    projectedSecondaryVoltage = x[3];
    assert(std::isfinite(projectedPrimaryCurrent) &&
           std::isfinite(projectedSecondaryCurrent) &&
           std::isfinite(projectedPrimaryVoltage) &&
           std::isfinite(projectedSecondaryVoltage));
  }

  return {projectedPrimaryVoltage, projectedSecondaryVoltage,
          projectedPrimaryCurrent, projectedSecondaryCurrent};
}

CurrentDrivenTransformerSample CurrentDrivenTransformer::process(
    float primaryDriveCurrentAmps,
    float secondaryLoadResistanceOhms,
    float primaryLoadResistanceOhms) {
  CurrentDrivenTransformerSample projected =
      project(primaryDriveCurrentAmps, secondaryLoadResistanceOhms,
              primaryLoadResistanceOhms);
  assert(std::isfinite(projected.primaryVoltage) &&
         std::isfinite(projected.secondaryVoltage) &&
         std::isfinite(projected.primaryCurrent) &&
         std::isfinite(projected.secondaryCurrent));
  primaryVoltage = projected.primaryVoltage;
  secondaryVoltage = projected.secondaryVoltage;
  primaryCurrent = projected.primaryCurrent;
  secondaryCurrent = projected.secondaryCurrent;
  return projected;
}

static inline float softClip(float x,
                             float t = kRadioSoftClipThresholdDefault) {
  float ax = std::fabs(x);
  if (ax <= t) return x;
  float s = (x < 0.0f) ? -1.0f : 1.0f;
  float u = (ax - t) / (1.0f - t);
  float y = t + (1.0f - std::exp(-u)) * (1.0f - t);
  return s * y;
}

void NoiseHum::setFs(float newFs, float noiseBwHz) {
  fs = newFs;
  noiseLpHz = (noiseBwHz > 0.0f) ? noiseBwHz : noiseLpHz;
  float safeLp = std::clamp(noiseLpHz, noiseHpHz + 200.0f, fs * 0.45f);
  hp.setHighpass(fs, noiseHpHz, filterQ);
  lp.setLowpass(fs, safeLp, filterQ);
  crackleHp.setHighpass(fs, noiseHpHz, filterQ);
  crackleLp.setLowpass(fs, safeLp, filterQ);
  hp.reset();
  lp.reset();
  crackleHp.reset();
  crackleLp.reset();
  scAtk = std::exp(-1.0f / (fs * (scAttackMs / 1000.0f)));
  scRel = std::exp(-1.0f / (fs * (scReleaseMs / 1000.0f)));
  crackleDecay = std::exp(-1.0f / (fs * (crackleDecayMs / 1000.0f)));
}

void NoiseHum::reset() {
  humPhase = 0.0f;
  scEnv = 0.0f;
  crackleEnv = 0.0f;
  pinkFast = 0.0f;
  pinkSlow = 0.0f;
  brown = 0.0f;
  hissDrift = 0.0f;
  hissDriftSlow = 0.0f;
  hp.reset();
  lp.reset();
  crackleHp.reset();
  crackleLp.reset();
}

float NoiseHum::process(const NoiseInput& in) {
  float programAbs = std::fabs(in.programSample);
  if (programAbs > scEnv) {
    scEnv = scAtk * scEnv + (1.0f - scAtk) * programAbs;
  } else {
    scEnv = scRel * scEnv + (1.0f - scRel) * programAbs;
  }
  float maskT = clampf(scEnv / sidechainMaskRef, 0.0f, 1.0f);
  float hissMask = 1.0f - hissMaskDepth * maskT;
  float burstMask = 1.0f - burstMaskDepth * maskT;

  float white = dist(rng);
  pinkFast = pinkFastPole * pinkFast + (1.0f - pinkFastPole) * white;
  pinkSlow = pinkSlowPole * pinkSlow + (1.0f - pinkSlowPole) * white;
  brown = clampf(brown + brownStep * white, -1.0f, 1.0f);
  hissDrift = hissDriftPole * hissDrift + hissDriftNoise * dist(rng);
  hissDriftSlow =
      hissDriftSlowPole * hissDriftSlow + hissDriftSlowNoise * dist(rng);
  float n = whiteMix * white + pinkFastMix * pinkFast +
            pinkDifferenceMix * (pinkSlow - pinkFastSubtract * pinkFast) +
            brownMix * brown;
  n *= hissBase + hissDriftDepth * hissDrift;
  n += hissDriftSlowMix * hissDriftSlow;
  n = hp.process(n);
  n = lp.process(n);
  n *= in.noiseAmp * hissMask;

  float c = 0.0f;
  if (in.crackleRate > 0.0f && in.crackleAmp > 0.0f && fs > 0.0f) {
    float chance = in.crackleRate / fs;
    if (dist01(rng) < chance) {
      crackleEnv = 1.0f;
    }
    float raw = dist(rng) * crackleEnv;
    crackleEnv *= crackleDecay;
    raw = crackleHp.process(raw);
    raw = crackleLp.process(raw);
    c = raw * in.crackleAmp * burstMask;
  }

  float h = 0.0f;
  if (in.humToneEnabled && in.humAmp > 0.0f && fs > 0.0f) {
    humPhase += kRadioTwoPi * (humHz / fs);
    if (humPhase > kRadioTwoPi) humPhase -= kRadioTwoPi;
    h = std::sin(humPhase) + humSecondHarmonicMix * std::sin(2.0f * humPhase);
    h *= in.humAmp * hissMask;
  }

  return n + c + h;
}

void AMDetector::init(float newFs, float newBw, float newTuneHz) {
  fs = newFs;
  bwHz = newBw;
  tuneOffsetHz = newTuneHz;
  float avcCap = avcFilterCapFarads;
  float avcChargeSeconds = avcChargeResistanceOhms * avcCap;
  float avcReleaseSeconds = avcDischargeResistanceOhms * avcCap;
  avcChargeCoeff = std::exp(-1.0f / (fs * avcChargeSeconds));
  avcReleaseCoeff = std::exp(-1.0f / (fs * avcReleaseSeconds));
  setBandwidth(newBw, newTuneHz);
  reset();
}

void AMDetector::setBandwidth(float newBw, float newTuneHz) {
  bwHz = newBw;
  tuneOffsetHz = newTuneHz;
  float audioPostLpHz = std::clamp(0.72f * std::max(bwHz, 1.0f), 1800.0f,
                                   std::min(7500.0f, 0.16f * fs));
  float ifCrackleTauSeconds =
      1.0f / (kRadioPi * std::max(bwHz, 1.0f));
  ifCrackleDecay =
      std::exp(-1.0f / (std::max(fs, 1.0f) *
                        std::max(ifCrackleTauSeconds, 1e-6f)));
  float lowSenseBound = std::max(40.0f, senseLowHz);
  float highSenseBound = std::max(lowSenseBound + 180.0f, senseHighHz);
  float ifCenter = 0.5f * (lowSenseBound + highSenseBound);
  float afcOffset =
      std::clamp(0.18f * (highSenseBound - lowSenseBound), 120.0f,
                 std::max(120.0f, 0.30f * (highSenseBound - lowSenseBound)));
  float lowSenseHz =
      std::clamp(ifCenter - afcOffset, lowSenseBound, highSenseBound - 180.0f);
  float highSenseHz =
      std::clamp(ifCenter + afcOffset, lowSenseHz + 120.0f, highSenseBound);
  float afcLpHz = std::clamp(0.30f * std::max(bwHz, 1.0f), 80.0f,
                             std::min(1800.0f, 0.12f * fs));
  afcLowOffsetHz = lowSenseHz - ifCenter;
  afcHighOffsetHz = highSenseHz - ifCenter;
  afcLowStep = kRadioTwoPi * (afcLowOffsetHz / std::max(fs, 1.0f));
  afcHighStep = kRadioTwoPi * (afcHighOffsetHz / std::max(fs, 1.0f));
  afcLowProbe.setLowpass(fs, afcLpHz, kRadioBiquadQ);
  afcHighProbe.setLowpass(fs, afcLpHz, kRadioBiquadQ);
  afcErrorLp.setLowpass(fs, afcLpHz, kRadioBiquadQ);
  audioPostLp1.setLowpass(fs, audioPostLpHz, kRadioBiquadQ);
}

void AMDetector::setSenseWindow(float lowHz, float highHz) {
  senseLowHz = lowHz;
  senseHighHz = highHz;
  if (fs > 0.0f) {
    setBandwidth(bwHz, tuneOffsetHz);
  }
}

void AMDetector::reset() {
  audioRect = 0.0f;
  avcRect = 0.0f;
  detectorNode = 0.0f;
  audioEnv = 0.0f;
  avcEnv = 0.0f;
  warmStartPending = true;
  afcError = 0.0f;
  ifCrackleEnv = 0.0f;
  ifCracklePhase = 0.0f;
  ifCrackleEventCount = 0;
  ifCrackleMaxBurstAmp = 0.0f;
  ifCrackleMaxEnv = 0.0f;
  afcLowPhase = 0.0f;
  afcHighPhase = 0.0f;
  afcLowSense.reset();
  afcHighSense.reset();
  afcLowProbe.reset();
  afcHighProbe.reset();
  afcErrorLp.reset();
  audioPostLp1.reset();
}

struct ReceiverInputNetworkSolve {
  float inputCurrent = 0.0f;
  float wiperVoltage = 0.0f;
  float tapVoltage = 0.0f;
  float gridVoltage = 0.0f;
  float couplingCapVoltage = 0.0f;
};

static std::array<float, 2> computeReceiverControlDcNodes(
    const Radio1938::ReceiverCircuitNodeState& receiver,
    float sourceVoltage);

static ReceiverInputNetworkSolve solveReceiverInputNetwork(
    const Radio1938::ReceiverCircuitNodeState& receiver,
    float dt,
    float sourceVoltage) {
  ReceiverInputNetworkSolve result{};
  float totalResistance = receiver.volumeControlResistanceOhms;
  float wiperResistance = receiver.volumeControlPosition * totalResistance;
  float tapResistance = receiver.volumeControlTapResistanceOhms;
  float loudnessBlend = 0.0f;
  if (tapResistance > 0.0f) {
    loudnessBlend = clampf((tapResistance - wiperResistance) / tapResistance,
                           0.0f, 1.0f);
  }

  constexpr float kNodeLinkOhms = 1e-3f;
  auto addGroundBranch = [](float (&a)[3][3], int node, float resistanceOhms) {
    if (resistanceOhms <= 0.0f) return;
    a[node][node] += 1.0f / std::max(resistanceOhms, 1e-9f);
  };
  auto addSourceBranch = [](float (&a)[3][3], float (&b)[3], int node,
                            float resistanceOhms, float sourceVoltageIn) {
    if (resistanceOhms <= 0.0f) return;
    float conductance = 1.0f / std::max(resistanceOhms, 1e-9f);
    a[node][node] += conductance;
    b[node] += conductance * sourceVoltageIn;
  };
  auto addNodeBranch = [](float (&a)[3][3], int aNode, int bNode,
                          float resistanceOhms) {
    if (resistanceOhms <= 0.0f) return;
    float conductance = 1.0f / std::max(resistanceOhms, 1e-9f);
    a[aNode][aNode] += conductance;
    a[bNode][bNode] += conductance;
    a[aNode][bNode] -= conductance;
    a[bNode][aNode] -= conductance;
  };

  float a[3][3] = {};
  float b[3] = {};
  constexpr int kWiperNode = 0;
  constexpr int kTapNode = 1;
  constexpr int kGridNode = 2;
  int sourceNode = kWiperNode;
  float sourceResistance = kNodeLinkOhms;
  if (wiperResistance >= tapResistance) {
    sourceNode = kWiperNode;
    sourceResistance =
        std::max(totalResistance - wiperResistance, kNodeLinkOhms);
    addSourceBranch(a, b, kWiperNode, sourceResistance, sourceVoltage);
    addNodeBranch(a, kWiperNode, kTapNode,
                  std::max(wiperResistance - tapResistance, kNodeLinkOhms));
    addGroundBranch(a, kTapNode, std::max(tapResistance, kNodeLinkOhms));
  } else {
    sourceNode = kTapNode;
    sourceResistance =
        std::max(totalResistance - tapResistance, kNodeLinkOhms);
    addSourceBranch(a, b, kTapNode, sourceResistance, sourceVoltage);
    addNodeBranch(a, kTapNode, kWiperNode,
                  std::max(tapResistance - wiperResistance, kNodeLinkOhms));
    addGroundBranch(a, kWiperNode, std::max(wiperResistance, kNodeLinkOhms));
  }
  if (loudnessBlend > 0.0f &&
      receiver.volumeControlLoudnessResistanceOhms > 0.0f) {
    a[kTapNode][kTapNode] +=
        loudnessBlend /
        std::max(receiver.volumeControlLoudnessResistanceOhms, 1e-9f);
  }
  if (loudnessBlend > 0.0f &&
      receiver.volumeControlLoudnessCapFarads > 0.0f) {
    float loudnessGc =
        loudnessBlend * receiver.volumeControlLoudnessCapFarads /
        std::max(dt, 1e-9f);
    a[kTapNode][kTapNode] += loudnessGc;
    b[kTapNode] += loudnessGc * receiver.volumeControlTapVoltage;
  }
  addGroundBranch(a, kGridNode, receiver.gridLeakResistanceOhms);
  float couplingGc = receiver.couplingCapFarads / std::max(dt, 1e-9f);
  a[kWiperNode][kWiperNode] += couplingGc;
  a[kGridNode][kGridNode] += couplingGc;
  a[kWiperNode][kGridNode] -= couplingGc;
  a[kGridNode][kWiperNode] -= couplingGc;
  b[kWiperNode] += couplingGc * receiver.couplingCapVoltage;
  b[kGridNode] -= couplingGc * receiver.couplingCapVoltage;
  float nodeVoltages[3] = {};
  bool solved = solveLinear3x3(a, b, nodeVoltages);
  assert(solved);
  result.wiperVoltage = nodeVoltages[kWiperNode];
  result.tapVoltage = nodeVoltages[kTapNode];
  result.gridVoltage = nodeVoltages[kGridNode];
  result.couplingCapVoltage =
      nodeVoltages[kWiperNode] - nodeVoltages[kGridNode];
  result.inputCurrent =
      (sourceVoltage - nodeVoltages[sourceNode]) /
      std::max(sourceResistance, kNodeLinkOhms);
  return result;
}

static float computeReceiverDetectorLoadConductance(
    const Radio1938::ReceiverCircuitNodeState& receiver) {
  if (!receiver.enabled) return 0.0f;

  float totalResistance =
      std::max(receiver.volumeControlResistanceOhms, 1e-6f);
  float volumePosition = clampf(receiver.volumeControlPosition, 0.0f, 1.0f);
  float wiperResistance = volumePosition * totalResistance;
  float sourceToWiperResistance =
      std::max(totalResistance - wiperResistance, 0.0f);

  // Reduced-order detector loading: the detector sees the full volume control
  // track to ground at all times, and above the coupling-cap corner it also
  // sees the first-audio grid-leak path through the top segment of the pot.
  // Keep this load explicit and physically interpretable, but avoid the older
  // dynamic Norton reduction here because its discrete companion branch was
  // measurably over-damping the detector and falsifying IMD/SINAD.
  float detectorLoadResistance = totalResistance;
  float gridLeakResistance = std::max(receiver.gridLeakResistanceOhms, 1e-6f);
  float gridPathResistance =
      std::max(sourceToWiperResistance + gridLeakResistance, 1e-6f);
  detectorLoadResistance =
      parallelResistance(detectorLoadResistance, gridPathResistance);

  if (receiver.volumeControlTapResistanceOhms > 0.0f &&
      receiver.volumeControlLoudnessResistanceOhms > 0.0f) {
    float tapResistance = receiver.volumeControlTapResistanceOhms;
    float loudnessBlend =
        clampf((tapResistance - wiperResistance) / tapResistance, 0.0f, 1.0f);
    if (loudnessBlend > 0.0f) {
      float loudnessResistance =
          std::max(receiver.volumeControlLoudnessResistanceOhms /
                       std::max(loudnessBlend, 1e-6f),
                   1e-6f);
      detectorLoadResistance =
          parallelResistance(detectorLoadResistance, loudnessResistance);
    }
  }

  return 1.0f / std::max(detectorLoadResistance, 1e-6f);
}

static void commitReceiverInputNetworkSolve(
    Radio1938::ReceiverCircuitNodeState& receiver,
    const ReceiverInputNetworkSolve& solve) {
  receiver.gridVoltage = solve.gridVoltage;
  receiver.volumeControlTapVoltage = solve.tapVoltage;
  receiver.couplingCapVoltage = solve.couplingCapVoltage;
  receiver.inputNetworkDrivenFromDetector = true;
}

void Radio1938::CalibrationStageMetrics::clearAccumulators() {
  sampleCount = 0;
  rmsIn = 0.0;
  rmsOut = 0.0;
  meanIn = 0.0;
  meanOut = 0.0;
  peakIn = 0.0f;
  peakOut = 0.0f;
  crestIn = 0.0f;
  crestOut = 0.0f;
  spectralCentroidHz = 0.0f;
  bandwidth3dBHz = 0.0f;
  bandwidth6dBHz = 0.0f;
  clipCountIn = 0;
  clipCountOut = 0;
  inSum = 0.0;
  outSum = 0.0;
  inSumSq = 0.0;
  outSumSq = 0.0;
  bandEnergy.fill(0.0);
  fftBinEnergy.fill(0.0);
  fftTimeBuffer.fill(0.0f);
  fftFill = 0;
  fftBlockCount = 0;
}

void Radio1938::CalibrationStageMetrics::resetMeasurementState() {
  fftTimeBuffer.fill(0.0f);
  fftFill = 0;
}

void Radio1938::CalibrationRmsMetric::reset() {
  sampleCount = 0;
  sumSq = 0.0;
  rms = 0.0f;
  peak = 0.0f;
}

void Radio1938::CalibrationRmsMetric::accumulate(float value) {
  if (!std::isfinite(value)) return;
  sampleCount++;
  sumSq += static_cast<double>(value) * static_cast<double>(value);
  peak = std::max(peak, std::fabs(value));
}

void Radio1938::CalibrationRmsMetric::updateSnapshot() {
  if (sampleCount == 0) {
    rms = 0.0f;
    return;
  }
  rms = static_cast<float>(
      std::sqrt(sumSq / static_cast<double>(sampleCount)));
}

static void fftInPlace(
    std::array<std::complex<float>, kRadioCalibrationFftSize>& bins) {
  const size_t n = bins.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(bins[i], bins[j]);
    }
  }

  for (size_t len = 2; len <= n; len <<= 1) {
    float angle = -kRadioTwoPi / static_cast<float>(len);
    std::complex<float> wLen(std::cos(angle), std::sin(angle));
    for (size_t i = 0; i < n; i += len) {
      std::complex<float> w(1.0f, 0.0f);
      size_t half = len >> 1;
      for (size_t j = 0; j < half; ++j) {
        std::complex<float> u = bins[i + j];
        std::complex<float> v = bins[i + j + half] * w;
        bins[i + j] = u + v;
        bins[i + j + half] = u - v;
        w *= wLen;
      }
    }
  }
}

static void accumulateCalibrationSpectrum(
    Radio1938::CalibrationStageMetrics& stage,
    float sampleRate,
    bool flushPartial) {
  if (stage.fftFill == 0) return;
  if (!flushPartial && stage.fftFill < kRadioCalibrationFftSize) return;

  std::array<std::complex<float>, kRadioCalibrationFftSize> bins{};
  const auto& window = radioCalibrationWindow();
  for (size_t i = 0; i < kRadioCalibrationFftSize; ++i) {
    float sample = (i < stage.fftFill) ? stage.fftTimeBuffer[i] : 0.0f;
    bins[i] = std::complex<float>(sample * window[i], 0.0f);
  }
  fftInPlace(bins);

  const auto& edges = radioCalibrationBandEdgesHz();
  float binHz = sampleRate / static_cast<float>(kRadioCalibrationFftSize);
  for (size_t i = 1; i < kRadioCalibrationFftBinCount; ++i) {
    float hz = static_cast<float>(i) * binHz;
    double energy = std::norm(bins[i]);
    stage.fftBinEnergy[i] += energy;
    for (size_t band = 0; band < kRadioCalibrationBandCount; ++band) {
      if (hz >= edges[band] && hz < edges[band + 1]) {
        stage.bandEnergy[band] += energy;
        break;
      }
    }
  }

  stage.fftBlockCount++;
  stage.fftTimeBuffer.fill(0.0f);
  stage.fftFill = 0;
}

void Radio1938::CalibrationState::reset() {
  totalSamples = 0;
  preLimiterClipCount = 0;
  postLimiterClipCount = 0;
  limiterActiveSamples = 0;
  limiterDutyCycle = 0.0f;
  limiterAverageGainReduction = 0.0f;
  limiterMaxGainReduction = 0.0f;
  limiterAverageGainReductionDb = 0.0f;
  limiterMaxGainReductionDb = 0.0f;
  limiterGainReductionSum = 0.0;
  limiterGainReductionDbSum = 0.0;
  validationSampleCount = 0;
  driverGridPositiveSamples = 0;
  outputGridPositiveSamples = 0;
  outputGridAPositiveSamples = 0;
  outputGridBPositiveSamples = 0;
  driverGridPositiveFraction = 0.0f;
  outputGridPositiveFraction = 0.0f;
  outputGridAPositiveFraction = 0.0f;
  outputGridBPositiveFraction = 0.0f;
  maxMixerPlateCurrentAmps = 0.0f;
  maxReceiverPlateCurrentAmps = 0.0f;
  maxDriverPlateCurrentAmps = 0.0f;
  maxOutputPlateCurrentAAmps = 0.0f;
  maxOutputPlateCurrentBAmps = 0.0f;
  interstageSecondarySumSq = 0.0;
  interstageSecondaryRmsVolts = 0.0f;
  interstageSecondaryPeakVolts = 0.0f;
  maxSpeakerSecondaryVolts = 0.0f;
  maxSpeakerReferenceRatio = 0.0f;
  maxDigitalOutput = 0.0f;
  detectorIfCrackleEventCount = 0;
  detectorIfCrackleMaxBurstAmp = 0.0f;
  detectorIfCrackleMaxEnv = 0.0f;
  detectorNodeVolts.reset();
  receiverGridVolts.reset();
  receiverPlateSwingVolts.reset();
  driverGridVolts.reset();
  driverPlateSwingVolts.reset();
  outputGridAVolts.reset();
  outputGridBVolts.reset();
  outputPrimaryVolts.reset();
  speakerSecondaryVolts.reset();
  validationDriverGridPositive = false;
  validationFailed = false;
  validationOutputGridPositive = false;
  validationOutputGridAPositive = false;
  validationOutputGridBPositive = false;
  validationSpeakerOverReference = false;
  validationInterstageSecondary = false;
  validationDcShift = false;
  validationDigitalClip = false;
  for (auto& stage : stages) {
    stage.clearAccumulators();
  }
}

void Radio1938::CalibrationState::resetMeasurementState() {
  for (auto& stage : stages) {
    stage.resetMeasurementState();
  }
}

static void updateStageCalibration(Radio1938& radio,
                                   StageId id,
                                   float in,
                                   float out) {
  if (!radio.calibration.enabled) return;
  auto& stage =
      radio.calibration.stages[static_cast<size_t>(id)];
  float clipThreshold = 1.0f;
  if (id == StageId::Power) {
    clipThreshold = std::max(radio.output.digitalReferenceSpeakerVoltsPeak, 1e-3f);
  }
  stage.sampleCount++;
  stage.inSum += static_cast<double>(in);
  stage.outSum += static_cast<double>(out);
  stage.inSumSq += static_cast<double>(in) * static_cast<double>(in);
  stage.outSumSq += static_cast<double>(out) * static_cast<double>(out);
  stage.peakIn = std::max(stage.peakIn, std::fabs(in));
  stage.peakOut = std::max(stage.peakOut, std::fabs(out));
  if (std::fabs(in) > clipThreshold) stage.clipCountIn++;
  if (std::fabs(out) > clipThreshold) stage.clipCountOut++;
  if (stage.fftFill < stage.fftTimeBuffer.size()) {
    stage.fftTimeBuffer[stage.fftFill++] = out;
  }
  if (stage.fftFill == stage.fftTimeBuffer.size()) {
    accumulateCalibrationSpectrum(stage, radio.sampleRate, false);
  }
}

void Radio1938::CalibrationStageMetrics::updateSnapshot(float sampleRate) {
  accumulateCalibrationSpectrum(*this, sampleRate, true);
  if (sampleCount == 0) return;
  double invCount = 1.0 / static_cast<double>(sampleCount);
  meanIn = inSum * invCount;
  meanOut = outSum * invCount;
  rmsIn = std::sqrt(inSumSq * invCount);
  rmsOut = std::sqrt(outSumSq * invCount);
  crestIn =
      (rmsIn > 1e-12) ? peakIn / static_cast<float>(rmsIn) : 0.0f;
  crestOut =
      (rmsOut > 1e-12) ? peakOut / static_cast<float>(rmsOut) : 0.0f;

  double totalEnergy = 0.0;
  double weightedHz = 0.0;
  double maxEnergy = 0.0;
  bandwidth3dBHz = 0.0f;
  bandwidth6dBHz = 0.0f;
  float binHz = sampleRate / static_cast<float>(kRadioCalibrationFftSize);
  for (size_t i = 1; i < fftBinEnergy.size(); ++i) {
    double energy = fftBinEnergy[i];
    float hz = static_cast<float>(i) * binHz;
    totalEnergy += energy;
    weightedHz += energy * hz;
    maxEnergy = std::max(maxEnergy, energy);
  }
  spectralCentroidHz = (totalEnergy > 1e-18) ? static_cast<float>(weightedHz / totalEnergy) : 0.0f;
  if (maxEnergy <= 0.0) return;

  double threshold3dB = maxEnergy * std::pow(10.0, -3.0 / 10.0);
  double threshold6dB = maxEnergy * std::pow(10.0, -6.0 / 10.0);
  for (size_t i = 1; i < fftBinEnergy.size(); ++i) {
    float hz = static_cast<float>(i) * binHz;
    if (fftBinEnergy[i] >= threshold3dB) {
      bandwidth3dBHz = hz;
    }
    if (fftBinEnergy[i] >= threshold6dB) {
      bandwidth6dBHz = hz;
    }
  }
}

static void updateCalibrationSnapshot(Radio1938& radio) {
  if (!radio.calibration.enabled) return;
  for (auto& stage : radio.calibration.stages) {
    stage.updateSnapshot(radio.sampleRate);
  }
  radio.calibration.detectorNodeVolts.updateSnapshot();
  radio.calibration.receiverGridVolts.updateSnapshot();
  radio.calibration.receiverPlateSwingVolts.updateSnapshot();
  radio.calibration.driverGridVolts.updateSnapshot();
  radio.calibration.driverPlateSwingVolts.updateSnapshot();
  radio.calibration.outputGridAVolts.updateSnapshot();
  radio.calibration.outputGridBVolts.updateSnapshot();
  radio.calibration.outputPrimaryVolts.updateSnapshot();
  radio.calibration.speakerSecondaryVolts.updateSnapshot();

  if (radio.calibration.totalSamples > 0) {
    float invCount = 1.0f / static_cast<float>(radio.calibration.totalSamples);
    radio.calibration.limiterDutyCycle =
        radio.calibration.limiterActiveSamples * invCount;
    radio.calibration.limiterAverageGainReduction =
        static_cast<float>(radio.calibration.limiterGainReductionSum * invCount);
    radio.calibration.limiterAverageGainReductionDb =
        static_cast<float>(radio.calibration.limiterGainReductionDbSum * invCount);
  }

  if (radio.calibration.validationSampleCount > 0) {
    float invValidationCount =
        1.0f / static_cast<float>(radio.calibration.validationSampleCount);
    radio.calibration.driverGridPositiveFraction =
        radio.calibration.driverGridPositiveSamples * invValidationCount;
    radio.calibration.outputGridPositiveFraction =
        radio.calibration.outputGridPositiveSamples * invValidationCount;
    radio.calibration.outputGridAPositiveFraction =
        radio.calibration.outputGridAPositiveSamples * invValidationCount;
    radio.calibration.outputGridBPositiveFraction =
        radio.calibration.outputGridBPositiveSamples * invValidationCount;
    radio.calibration.interstageSecondaryRmsVolts =
        std::sqrt(radio.calibration.interstageSecondarySumSq *
                  invValidationCount);
  }

  const auto& receiverStage =
      radio.calibration.stages[static_cast<size_t>(StageId::ReceiverCircuit)];
  const auto& powerStage =
      radio.calibration.stages[static_cast<size_t>(StageId::Power)];
  radio.calibration.detectorIfCrackleEventCount =
      radio.demod.am.ifCrackleEventCount;
  radio.calibration.detectorIfCrackleMaxBurstAmp =
      radio.demod.am.ifCrackleMaxBurstAmp;
  radio.calibration.detectorIfCrackleMaxEnv = radio.demod.am.ifCrackleMaxEnv;
  radio.calibration.validationDriverGridPositive =
      radio.calibration.driverGridPositiveFraction > 0.01f;
  radio.calibration.validationOutputGridAPositive =
      radio.calibration.outputGridAPositiveFraction > 0.002f;
  radio.calibration.validationOutputGridBPositive =
      radio.calibration.outputGridBPositiveFraction > 0.002f;
  radio.calibration.validationOutputGridPositive =
      radio.calibration.validationOutputGridAPositive ||
      radio.calibration.validationOutputGridBPositive;
  radio.calibration.validationSpeakerOverReference =
      radio.calibration.maxSpeakerReferenceRatio > 1.10f;
  radio.calibration.validationInterstageSecondary =
      radio.calibration.interstageSecondaryPeakVolts >
      4.0f * std::max(std::fabs(radio.power.outputTubeBiasVolts), 1.0f);
  radio.calibration.validationDcShift =
      (std::fabs(receiverStage.meanOut) >
           std::max(0.02, 0.10 * receiverStage.rmsOut)) ||
      (std::fabs(powerStage.meanOut) >
           std::max(0.10, 0.05 * powerStage.peakOut));
  radio.calibration.validationDigitalClip =
      radio.calibration.maxDigitalOutput > 1.0f ||
      radio.diagnostics.outputClip || radio.diagnostics.finalLimiterActive;
  radio.calibration.validationFailed =
      radio.calibration.validationDriverGridPositive ||
      radio.calibration.validationOutputGridPositive ||
      radio.calibration.validationSpeakerOverReference ||
      radio.calibration.validationInterstageSecondary ||
      radio.calibration.validationDcShift ||
      radio.calibration.validationDigitalClip;
}

float AMDetector::processEnvelope(float signalI,
                                  float signalQ,
                                  float ifNoiseAmp,
                                  Radio1938& radio,
                                  float ifCrackleAmp,
                                  float ifCrackleRate) {
  constexpr float kInvSqrt2 = 0.70710678118f;
  float ifI = signalI;
  float ifQ = signalQ;
  if (ifNoiseAmp > 0.0f) {
    float noiseScale = ifNoiseAmp * kInvSqrt2;
    ifI += dist(rng) * noiseScale;
    ifQ += dist(rng) * noiseScale;
  }
  if (ifCrackleAmp > 0.0f && ifCrackleRate > 0.0f && fs > 0.0f) {
    float chance = std::min(ifCrackleRate / fs, 1.0f);
    float eventDraw = 0.5f * (dist(rng) + 1.0f);
    if (eventDraw < chance) {
      // Impulsive RF interference reaches the detector through the tuned IF
      // path, so model it as a short ring-down in the complex envelope instead
      // of a post-audio click.
      float burstPhase = kRadioPi * (dist(rng) + 1.0f);
      float burstAmpDraw = 0.5f * (dist(rng) + 1.0f);
      float burstAmp = ifCrackleAmp * (0.35f + 0.65f * burstAmpDraw);
      ifCrackleEnv = std::max(ifCrackleEnv, burstAmp);
      ifCrackleEventCount++;
      ifCrackleMaxBurstAmp = std::max(ifCrackleMaxBurstAmp, burstAmp);
      ifCracklePhase = burstPhase;
    }
  }
  if (ifCrackleEnv > 1e-6f) {
    ifCrackleMaxEnv = std::max(ifCrackleMaxEnv, ifCrackleEnv);
    ifI += ifCrackleEnv * std::cos(ifCracklePhase);
    ifQ += ifCrackleEnv * std::sin(ifCracklePhase);
    ifCrackleEnv *= ifCrackleDecay;
  }

  auto processProbe = [&](float phase,
                          float step,
                          IQBiquad& probe,
                          float& nextPhase) {
    float c = std::cos(phase);
    float s = std::sin(phase);
    float mixedI = ifI * c + ifQ * s;
    float mixedQ = ifQ * c - ifI * s;
    auto filtered = probe.process(mixedI, mixedQ);
    nextPhase = wrapPhase(phase + step);
    return std::sqrt(filtered[0] * filtered[0] + filtered[1] * filtered[1]);
  };

  float nextLowPhase = afcLowPhase;
  float nextHighPhase = afcHighPhase;
  float afcLow =
      processProbe(afcLowPhase, afcLowStep, afcLowProbe, nextLowPhase);
  float afcHigh =
      processProbe(afcHighPhase, afcHighStep, afcHighProbe, nextHighPhase);
  afcLowPhase = nextLowPhase;
  afcHighPhase = nextHighPhase;

  float afcDen = std::max(afcLow + afcHigh, 1e-6f);
  float rawAfcError = (afcHigh - afcLow) / afcDen;
  afcError = afcErrorLp.process(rawAfcError);

  float ifMagnitude = std::sqrt(ifI * ifI + ifQ * ifQ);
  audioRect = diodeJunctionRectify(ifMagnitude, audioDiodeDrop,
                                   audioJunctionSlopeVolts);
  float delayedAvcThreshold = 0.18f * std::max(controlVoltageRef, 1e-6f);
  float dt = 1.0f / std::max(fs, 1.0f);
  bool useReceiverLoad = radio.receiverCircuit.enabled;
  if (warmStartPending) {
    detectorNode = audioRect;
    avcEnv = std::max(detectorNode - delayedAvcThreshold, 0.0f);
    avcRect = avcEnv;
    auto prechargeUnityLowpass = [](Biquad& biquad, float dcLevel) {
      biquad.z1 = dcLevel * (1.0f - biquad.b0);
      biquad.z2 = dcLevel * (biquad.b2 - biquad.a2);
    };
    prechargeUnityLowpass(audioPostLp1, detectorNode);
    warmStartPending = false;
  }
  if (useReceiverLoad && radio.receiverCircuit.warmStartPending) {
    auto dcNodes =
        computeReceiverControlDcNodes(radio.receiverCircuit, detectorNode);
    radio.receiverCircuit.volumeControlTapVoltage = dcNodes[1];
    radio.receiverCircuit.couplingCapVoltage = dcNodes[0];
    radio.receiverCircuit.gridVoltage = 0.0f;
    radio.receiverCircuit.warmStartPending = false;
  }
  float storageCapG =
      std::max(detectorStorageCapFarads, 1e-12f) / dt;
  float detectorLeakG =
      1.0f / std::max(audioDischargeResistanceOhms, 1e-6f);
  float sourceG = 0.0f;
  if (audioRect > detectorNode) {
    sourceG = 1.0f / std::max(audioChargeResistanceOhms, 1e-6f);
  }

  float avcFeedG = 0.0f;
  if (std::max(audioRect, detectorNode) > delayedAvcThreshold) {
    avcFeedG = 1.0f / std::max(avcChargeResistanceOhms, 1e-6f);
  }
  float avcLeakG = 1.0f / std::max(avcDischargeResistanceOhms, 1e-6f);
  float avcCapG = std::max(avcFilterCapFarads, 1e-12f) / dt;
  float a00 = storageCapG + detectorLeakG + sourceG + avcFeedG;
  float a01 = -avcFeedG;
  float a10 = -avcFeedG;
  float a11 = avcFeedG + avcLeakG + avcCapG;
  float b0 = storageCapG * detectorNode + sourceG * audioRect -
             avcFeedG * delayedAvcThreshold;
  float b1 = avcCapG * avcEnv + avcFeedG * delayedAvcThreshold;
  float solvedDetectorNode = detectorNode;
  float solvedAvcNode = avcEnv;
  if (useReceiverLoad) {
    a00 += computeReceiverDetectorLoadConductance(radio.receiverCircuit);
  }
  float det = a00 * a11 - a01 * a10;
  assert(std::fabs(det) >= 1e-12f);
  solvedDetectorNode = (b0 * a11 - a01 * b1) / det;
  solvedAvcNode = (a00 * b1 - b0 * a10) / det;
  detectorNode = std::max(solvedDetectorNode, 0.0f);
  if (useReceiverLoad) {
    auto receiverSolve =
        solveReceiverInputNetwork(radio.receiverCircuit, dt, detectorNode);
    commitReceiverInputNetworkSolve(radio.receiverCircuit, receiverSolve);
  }
  avcEnv = std::max(solvedAvcNode, 0.0f);
  avcRect = avcEnv;
  assert(std::isfinite(detectorNode) && std::isfinite(avcEnv));
  if (radio.calibration.enabled) {
    radio.calibration.detectorNodeVolts.accumulate(detectorNode);
  }

  audioEnv = audioPostLp1.process(detectorNode);
  return audioEnv;
}

float AMDetector::process(const AMDetectorSampleInput& in, Radio1938& radio) {
  return processEnvelope(in.signal, 0.0f, in.ifNoiseAmp, radio,
                         in.ifCrackleAmp, in.ifCrackleRate);
}

void SpeakerSim::init(float fs) {
  float suspensionHzDerived =
      suspensionHz * (1.0f + 0.45f * coneMassTolerance -
                      0.65f * suspensionComplianceTolerance);
  float coneBodyHzDerived =
      coneBodyHz * (1.0f + 0.22f * coneMassTolerance +
                    0.16f * voiceCoilTolerance);
  suspensionRes.setPeaking(fs, suspensionHzDerived, suspensionQ, suspensionGainDb);
  coneBody.setPeaking(fs, coneBodyHzDerived, coneBodyQ, coneBodyGainDb);
  upperBreakup = Biquad{};
  coneDip = Biquad{};
  if (topLpHz > 0.0f) {
    float topLpHzDerived = topLpHz / (1.0f + 0.40f * voiceCoilTolerance);
    topLp.setLowpass(fs, topLpHzDerived, filterQ);
  } else {
    topLp = Biquad{};
  }
  hfLossLp = Biquad{};
  excursionAtk = std::exp(-1.0f / (fs * 0.010f));
  excursionRel = std::exp(-1.0f / (fs * 0.120f));
}

void SpeakerSim::reset() {
  suspensionRes.reset();
  coneBody.reset();
  upperBreakup.reset();
  coneDip.reset();
  topLp.reset();
  hfLossLp.reset();
  excursionEnv = 0.0f;
}

float SpeakerSim::process(float x, bool& clipped) {
  float y = x * std::max(drive, 0.0f);
  y = suspensionRes.process(y);
  y = coneBody.process(y);
  if (topLpHz > 0.0f) {
    y = topLp.process(y);
  }

  float a = std::fabs(y);
  if (a > excursionEnv) {
    excursionEnv = excursionAtk * excursionEnv + (1.0f - excursionAtk) * a;
  } else {
    excursionEnv = excursionRel * excursionEnv + (1.0f - excursionRel) * a;
  }

  float excursionT =
      clampf(excursionEnv / std::max(excursionRef, 1e-6f), 0.0f, 1.0f);
  float complianceGain = 1.0f - complianceLossDepth * excursionT;
  y *= std::max(0.70f, complianceGain);
  clipped = limit > 0.0f && std::fabs(y) > limit;
  if (limit > 0.0f && limit < 1.0f) {
    return softClip(y, limit);
  }
  return y;
}

float RadioTuningNode::applyFilters(Radio1938& radio, float tuneHz, float bwHz) {
  auto& tuning = radio.tuning;
  auto& frontEnd = radio.frontEnd;
  float safeAudioBw = bwHz;
  float physicalChannelBw = 2.0f * safeAudioBw;
  float preBw = physicalChannelBw * tuning.preBwScale;
  float rfBw = physicalChannelBw * tuning.postBwScale;
  RadioIFStripNode::setBandwidth(radio, safeAudioBw, tuneHz);
  float rfCenterHz = radio.ifStrip.sourceCarrierHz;
  auto resonantCapacitanceFarads = [](float freqHz, float inductanceHenries) {
    float omega = kRadioTwoPi * std::max(freqHz, 1.0f);
    return 1.0f / std::max(omega * omega * std::max(inductanceHenries, 1e-9f),
                           1e-18f);
  };
  auto seriesResistanceForBandwidth = [](float inductanceHenries,
                                         float bandwidthHz) {
    return std::max(kRadioTwoPi * std::max(inductanceHenries, 1e-9f) *
                        std::max(bandwidthHz, 1.0f),
                    1e-4f);
  };
  float antennaDrift = std::max(radio.identity.frontEndAntennaDrift, 0.5f);
  float rfDrift = std::max(radio.identity.frontEndRfDrift, 0.5f);
  float antennaInductance = frontEnd.antennaInductanceHenries * antennaDrift;
  float rfInductance = frontEnd.rfInductanceHenries * rfDrift;
  float antennaLoadResistance =
      frontEnd.antennaLoadResistanceOhms * (2.0f - antennaDrift);
  float rfLoadResistance = frontEnd.rfLoadResistanceOhms * (2.0f - rfDrift);

  frontEnd.antennaCapacitanceFarads =
      resonantCapacitanceFarads(rfCenterHz, antennaInductance);
  frontEnd.rfCapacitanceFarads =
      resonantCapacitanceFarads(rfCenterHz, rfInductance);
  frontEnd.antennaSeriesResistanceOhms =
      seriesResistanceForBandwidth(antennaInductance, preBw);
  frontEnd.rfSeriesResistanceOhms =
      seriesResistanceForBandwidth(rfInductance, rfBw);
  frontEnd.antennaTank.configure(
      radio.sampleRate, antennaInductance,
      frontEnd.antennaCapacitanceFarads,
      frontEnd.antennaSeriesResistanceOhms + antennaLoadResistance,
      antennaLoadResistance, 8);
  frontEnd.rfTank.configure(radio.sampleRate, rfInductance,
                            frontEnd.rfCapacitanceFarads,
                            frontEnd.rfSeriesResistanceOhms + rfLoadResistance,
                            rfLoadResistance, 8);

  frontEnd.preLpfIn.setBandpass(
      radio.sampleRate, rfCenterHz, std::max(0.35f, rfCenterHz / preBw));
  frontEnd.preLpfOut.setBandpass(
      radio.sampleRate, rfCenterHz, std::max(0.35f, rfCenterHz / rfBw));

  tuning.tunedBw = safeAudioBw;
  return safeAudioBw;
}

void RadioTuningNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& tuning = radio.tuning;
  tuning.afcCorrectionHz = 0.0f;
  initCtx.tunedBw = applyFilters(radio, tuning.tuneOffsetHz, radio.bwHz);
  tuning.tuneAppliedHz = tuning.tuneOffsetHz;
  tuning.bwAppliedHz = radio.bwHz;
  tuning.tuneSmoothedHz = tuning.tuneOffsetHz;
  tuning.bwSmoothedHz = radio.bwHz;
}

void RadioTuningNode::reset(Radio1938& radio) {
  auto& tuning = radio.tuning;
  tuning.afcCorrectionHz = 0.0f;
  tuning.tuneAppliedHz = tuning.tuneOffsetHz;
  tuning.bwAppliedHz = radio.bwHz;
  tuning.tuneSmoothedHz = tuning.tuneOffsetHz;
  tuning.bwSmoothedHz = radio.bwHz;
}

void RadioTuningNode::prepare(Radio1938& radio,
                              RadioBlockControl& block,
                              uint32_t frames) {
  auto& tuning = radio.tuning;
  auto& demod = radio.demod;
  auto& noiseRuntime = radio.noiseRuntime;
  float rate = std::max(1.0f, radio.sampleRate);
  float tick =
      1.0f - std::exp(-static_cast<float>(frames) / (rate * tuning.smoothTau));
  float effectiveTuneHz = tuning.tuneOffsetHz;
  if (tuning.magneticTuningEnabled) {
    effectiveTuneHz += tuning.afcCorrectionHz;
  }
  tuning.tuneSmoothedHz += tick * (effectiveTuneHz - tuning.tuneSmoothedHz);
  tuning.bwSmoothedHz += tick * (radio.bwHz - tuning.bwSmoothedHz);

  float safeBw = tuning.bwSmoothedHz;
  float bwHalf = 0.5f * std::max(1.0f, safeBw);
  block.tuneNorm = clampf(tuning.tuneSmoothedHz / bwHalf, -1.0f, 1.0f);

  if (std::fabs(tuning.tuneSmoothedHz - tuning.tuneAppliedHz) > tuning.updateEps ||
      std::fabs(tuning.bwSmoothedHz - tuning.bwAppliedHz) > tuning.updateEps) {
    float tunedBw = applyFilters(radio, tuning.tuneSmoothedHz, tuning.bwSmoothedHz);
    tuning.tuneAppliedHz = tuning.tuneSmoothedHz;
    tuning.bwAppliedHz = tuning.bwSmoothedHz;
    demod.am.setBandwidth(tunedBw, tuning.tuneSmoothedHz);
    noiseRuntime.hum.setFs(radio.sampleRate, tunedBw);
  }
}

void RadioInputNode::init(Radio1938& radio, RadioInitContext&) {
  auto& input = radio.input;
  input.autoEnvAtk =
      std::exp(-1.0f / (radio.sampleRate * (input.autoEnvAttackMs / 1000.0f)));
  input.autoEnvRel =
      std::exp(-1.0f / (radio.sampleRate * (input.autoEnvReleaseMs / 1000.0f)));
  input.autoGainAtk =
      std::exp(-1.0f / (radio.sampleRate * (input.autoGainAttackMs / 1000.0f)));
  input.autoGainRel =
      std::exp(-1.0f / (radio.sampleRate * (input.autoGainReleaseMs / 1000.0f)));
  float sourceR = input.sourceResistanceOhms;
  float loadR = input.inputResistanceOhms;
  input.sourceDivider = loadR / (sourceR + loadR);
  if (input.couplingCapFarads > 0.0f) {
    float hpHz = 1.0f / (kRadioTwoPi * (sourceR + loadR) * input.couplingCapFarads);
    input.sourceCouplingHp.setHighpass(radio.sampleRate, hpHz, kRadioBiquadQ);
  } else {
    input.sourceCouplingHp = Biquad{};
  }
}

void RadioInputNode::reset(Radio1938& radio) {
  radio.input.autoEnv = 0.0f;
  radio.input.autoGainDb = 0.0f;
  radio.input.sourceCouplingHp.reset();
}

float RadioInputNode::process(Radio1938& radio,
                              float x,
                              const RadioSampleContext&) {
  auto& input = radio.input;
  if (radio.sourceFrame.mode == SourceInputMode::RealRf) {
    x *= input.sourceDivider;
    if (input.couplingCapFarads > 0.0f) {
      x = input.sourceCouplingHp.process(x);
    }
  }
  x *= radio.globals.inputPad;
  if (!radio.globals.enableAutoLevel) return x;

  float ax = std::fabs(x);
  if (ax > input.autoEnv) {
    input.autoEnv = input.autoEnvAtk * input.autoEnv + (1.0f - input.autoEnvAtk) * ax;
  } else {
    input.autoEnv = input.autoEnvRel * input.autoEnv + (1.0f - input.autoEnvRel) * ax;
  }
  float envDb = lin2db(input.autoEnv);
  float targetBoostDb =
      std::clamp(radio.globals.autoTargetDb - envDb, 0.0f,
                 radio.globals.autoMaxBoostDb);
  if (targetBoostDb < input.autoGainDb) {
    input.autoGainDb = input.autoGainAtk * input.autoGainDb +
                       (1.0f - input.autoGainAtk) * targetBoostDb;
  } else {
    input.autoGainDb = input.autoGainRel * input.autoGainDb +
                       (1.0f - input.autoGainRel) * targetBoostDb;
  }
  return x * db2lin(input.autoGainDb);
}

void RadioControlBusNode::init(Radio1938&, RadioInitContext&) {}

void RadioControlBusNode::reset(Radio1938& radio) {
  radio.controlSense.reset();
  radio.controlBus.reset();
}

void RadioAVCNode::update(Radio1938& radio, RadioSampleContext&) {
  auto& controlBus = radio.controlBus;
  const auto& controlSense = radio.controlSense;
  controlBus.controlVoltage =
      clampf(controlSense.controlVoltageSense, 0.0f, 1.25f);
}

void RadioAFCNode::update(Radio1938& radio, RadioSampleContext&) {
  auto& tuning = radio.tuning;
  const auto& controlSense = radio.controlSense;
  if (!tuning.magneticTuningEnabled || tuning.afcMaxCorrectionHz <= 0.0f ||
      tuning.afcResponseMs <= 0.0f) {
    tuning.afcCorrectionHz = 0.0f;
    return;
  }

  float rate = std::max(radio.sampleRate, 1.0f);
  float afcSeconds = tuning.afcResponseMs * 0.001f;
  float afcTick = 1.0f - std::exp(-1.0f / (rate * afcSeconds));
  float error = controlSense.tuningErrorSense;
  if (std::fabs(error) < tuning.afcDeadband) error = 0.0f;
  float captureT =
      1.0f - clampf(std::fabs(tuning.tuneOffsetHz) /
                        std::max(tuning.afcCaptureHz, 1e-6f),
                    0.0f, 1.0f);
  float signalT =
      clampf(controlSense.controlVoltageSense / 0.85f, 0.0f, 1.0f);
  float afcTarget =
      -error * tuning.afcMaxCorrectionHz * captureT * signalT;
  tuning.afcCorrectionHz += afcTick * (afcTarget - tuning.afcCorrectionHz);
  tuning.afcCorrectionHz =
      clampf(tuning.afcCorrectionHz, -tuning.afcMaxCorrectionHz,
             tuning.afcMaxCorrectionHz);
}

void RadioControlBusNode::update(Radio1938& radio, RadioSampleContext&) {
  auto& controlBus = radio.controlBus;
  const auto& controlSense = radio.controlSense;
  const auto& power = radio.power;
  float supplyTarget =
      clampf((controlSense.powerSagSense - power.sagStart) /
                 std::max(1e-6f, power.sagEnd - power.sagStart),
             0.0f, 1.0f);
  controlBus.supplySag = supplyTarget;
}

void RadioFrontEndNode::init(Radio1938& radio, RadioInitContext&) {
  radio.frontEnd.hpf.setHighpass(radio.sampleRate, radio.frontEnd.inputHpHz,
                                 kRadioBiquadQ);
  radio.frontEnd.selectivityPeak.setPeaking(radio.sampleRate,
                                            radio.frontEnd.selectivityPeakHz,
                                            radio.frontEnd.selectivityPeakQ,
                                            radio.frontEnd.selectivityPeakGainDb);
}

void RadioFrontEndNode::reset(Radio1938& radio) {
  auto& frontEnd = radio.frontEnd;
  frontEnd.hpf.reset();
  frontEnd.preLpfIn.reset();
  frontEnd.preLpfOut.reset();
  frontEnd.selectivityPeak.reset();
  frontEnd.antennaTank.reset();
  frontEnd.rfTank.reset();
}

float RadioFrontEndNode::process(Radio1938& radio,
                                 float x,
                                 const RadioSampleContext&) {
  auto& frontEnd = radio.frontEnd;
  float rfHold = clampf(radio.controlBus.controlVoltage / 1.25f, 0.0f, 1.0f);
  float y = frontEnd.hpf.process(x);
  if (radio.sourceFrame.mode == SourceInputMode::RealRf) {
    y = frontEnd.preLpfIn.process(y);
  }
  y = frontEnd.antennaTank.process(y);
  y *= frontEnd.rfGain * std::max(0.35f, 1.0f - frontEnd.avcGainDepth * rfHold);
  y = frontEnd.rfTank.process(y);
  if (radio.sourceFrame.mode == SourceInputMode::RealRf) {
    y = frontEnd.preLpfOut.process(y);
  }
  y = frontEnd.selectivityPeak.process(y);
  return y;
}

static float estimateMixerEnvelopeConversionGain(
    const FixedPlatePentodeEvaluator& plateCurrentForGrid,
    float mixedBaseGridVolts,
    float loGridDriveVolts);

void RadioMixerNode::init(Radio1938& radio, RadioInitContext&) {
  auto& mixer = radio.mixer;
  float dcLoadResistanceOhms = 0.0f;
  if (mixer.plateCurrentAmps > 1e-6f &&
      mixer.plateSupplyVolts > mixer.plateDcVolts) {
    dcLoadResistanceOhms =
        (mixer.plateSupplyVolts - mixer.plateDcVolts) / mixer.plateCurrentAmps;
  }
  PentodeOperatingPoint op = solvePentodeOperatingPoint(
      mixer.plateSupplyVolts, mixer.screenVolts, dcLoadResistanceOhms,
      mixer.biasVolts, mixer.plateDcVolts, mixer.plateCurrentAmps,
      mixer.mutualConductanceSiemens, mixer.plateKneeVolts,
      mixer.gridSoftnessVolts, mixer.modelCutoffVolts);
  mixer.plateQuiescentVolts = op.plateVolts;
  mixer.plateQuiescentCurrentAmps = op.plateCurrentAmps;
  mixer.plateResistanceOhms = op.rpOhms;
  assert((std::fabs(mixer.plateQuiescentVolts - mixer.plateDcVolts) <=
          mixer.operatingPointToleranceVolts) &&
         "Mixer operating point diverged from the preset target");
}

void RadioMixerNode::reset(Radio1938& radio) {
  radio.mixer.mixedBaseGridVolts = 0.0f;
  radio.mixer.conversionGain = 1.0f;
  radio.mixer.inputDriveEnv = 0.0f;
}

float RadioMixerNode::process(Radio1938& radio,
                              float y,
                              const RadioSampleContext&) {
  auto& mixer = radio.mixer;
  float avcT = clampf(radio.controlBus.controlVoltage / 1.25f, 0.0f, 1.0f);
  float rfGridDrive =
      mixer.rfGridDriveVolts * std::max(radio.identity.mixerDriveDrift, 0.65f);
  float loGridBias =
      mixer.loGridBiasVolts * std::max(radio.identity.mixerBiasDrift, 0.65f);
  float baseGridVolts = mixer.biasVolts - mixer.avcGridDriveVolts * avcT;
  mixer.mixedBaseGridVolts = baseGridVolts + loGridBias;
  FixedPlatePentodeEvaluator mixerPlateCurrentForGrid =
      prepareFixedPlatePentodeEvaluator(
          mixer.plateQuiescentVolts, mixer.screenVolts, mixer.biasVolts,
          mixer.modelCutoffVolts, mixer.plateQuiescentVolts, mixer.screenVolts,
          mixer.plateQuiescentCurrentAmps, mixer.mutualConductanceSiemens,
          mixer.plateKneeVolts, mixer.gridSoftnessVolts);
  mixer.conversionGain =
      estimateMixerEnvelopeConversionGain(mixerPlateCurrentForGrid,
                                          mixer.mixedBaseGridVolts,
                                          mixer.loGridDriveVolts) *
      rfGridDrive * mixer.acLoadResistanceOhms;

  float signalMagnitude = std::fabs(y);
  if (radio.sourceFrame.mode == SourceInputMode::ComplexEnvelope) {
    signalMagnitude = std::sqrt(radio.sourceFrame.i * radio.sourceFrame.i +
                                radio.sourceFrame.q * radio.sourceFrame.q);
  }
  float sampleRate = std::max(radio.sampleRate, 1.0f);
  float envCoeff = std::exp(-1.0f / (sampleRate * 0.0015f));
  mixer.inputDriveEnv =
      envCoeff * mixer.inputDriveEnv + (1.0f - envCoeff) * signalMagnitude;

  if (radio.sourceFrame.mode == SourceInputMode::ComplexEnvelope) {
    return radio.sourceFrame.i;
  }

  return y;
}

static float selectSourceCarrierHz(float outputFs,
                                   float internalFs,
                                   float ifCenterHz,
                                   float bwHz) {
  float maxCarrierByIf = 0.48f * internalFs - ifCenterHz - 8000.0f;
  float audioSidebandHz = 0.48f * std::max(bwHz, 1.0f);
  float maxCarrierByOutput =
      0.5f * std::max(outputFs, 1.0f) - audioSidebandHz - 1600.0f;
  float maxCarrier = maxCarrierByOutput;
  if (ifCenterHz > 0.0f) {
    maxCarrier = std::min(maxCarrierByIf, maxCarrierByOutput);
  }
  if (maxCarrier <= 6000.0f) {
    return std::clamp(0.25f * std::max(outputFs, 1.0f), 3000.0f,
                      std::max(3000.0f, maxCarrierByOutput));
  }
  return std::clamp(0.62f * maxCarrier, 6000.0f, maxCarrier);
}

static inline std::array<float, 2> rotateComplexEnvelope(float inI,
                                                         float inQ,
                                                         float phase);

static inline std::array<float, 2> unrotateComplexEnvelope(float inI,
                                                           float inQ,
                                                           float phase);

static std::array<float, 2> computeReceiverControlDcNodes(
    const Radio1938::ReceiverCircuitNodeState& receiver,
    float sourceVoltage) {
  float totalResistance = receiver.volumeControlResistanceOhms;
  float wiperResistance = receiver.volumeControlPosition * totalResistance;
  float tapResistance = receiver.volumeControlTapResistanceOhms;
  float loudnessBlend = 0.0f;
  if (tapResistance > 0.0f) {
    loudnessBlend = clampf((tapResistance - wiperResistance) / tapResistance,
                           0.0f, 1.0f);
  }

  constexpr float kNodeLinkOhms = 1e-3f;
  auto addGroundBranch = [](float (&a)[2][2], int node, float resistanceOhms) {
    if (resistanceOhms <= 0.0f) return;
    a[node][node] += 1.0f / std::max(resistanceOhms, 1e-9f);
  };
  auto addSourceBranch = [](float (&a)[2][2], float (&b)[2], int node,
                            float resistanceOhms, float sourceVoltageIn) {
    if (resistanceOhms <= 0.0f) return;
    float conductance = 1.0f / std::max(resistanceOhms, 1e-9f);
    a[node][node] += conductance;
    b[node] += conductance * sourceVoltageIn;
  };
  auto addNodeBranch = [](float (&a)[2][2], int aNode, int bNode,
                          float resistanceOhms) {
    if (resistanceOhms <= 0.0f) return;
    float conductance = 1.0f / std::max(resistanceOhms, 1e-9f);
    a[aNode][aNode] += conductance;
    a[bNode][bNode] += conductance;
    a[aNode][bNode] -= conductance;
    a[bNode][aNode] -= conductance;
  };

  float a[2][2] = {};
  float b[2] = {};
  constexpr int kWiperNode = 0;
  constexpr int kTapNode = 1;
  if (wiperResistance >= tapResistance) {
    addSourceBranch(a, b, kWiperNode,
                    std::max(totalResistance - wiperResistance, kNodeLinkOhms),
                    sourceVoltage);
    addNodeBranch(a, kWiperNode, kTapNode,
                  std::max(wiperResistance - tapResistance, kNodeLinkOhms));
    addGroundBranch(a, kTapNode, std::max(tapResistance, kNodeLinkOhms));
  } else {
    addSourceBranch(a, b, kTapNode,
                    std::max(totalResistance - tapResistance, kNodeLinkOhms),
                    sourceVoltage);
    addNodeBranch(a, kTapNode, kWiperNode,
                  std::max(tapResistance - wiperResistance, kNodeLinkOhms));
    addGroundBranch(a, kWiperNode, std::max(wiperResistance, kNodeLinkOhms));
  }
  if (loudnessBlend > 0.0f &&
      receiver.volumeControlLoudnessResistanceOhms > 0.0f) {
    a[kTapNode][kTapNode] +=
        loudnessBlend /
        std::max(receiver.volumeControlLoudnessResistanceOhms, 1e-9f);
  }

  float det = a[0][0] * a[1][1] - a[0][1] * a[1][0];
  assert(std::fabs(det) >= 1e-12f);
  float wiperVoltage = (b[0] * a[1][1] - a[0][1] * b[1]) / det;
  float tapVoltage = (a[0][0] * b[1] - b[0] * a[1][0]) / det;
  assert(std::isfinite(wiperVoltage) && std::isfinite(tapVoltage));
  return {wiperVoltage, tapVoltage};
}

void RadioIFStripNode::init(Radio1938& radio, RadioInitContext&) {
  setBandwidth(radio, radio.bwHz, radio.tuning.tuneOffsetHz);
}

void RadioIFStripNode::reset(Radio1938& radio) {
  auto& ifStrip = radio.ifStrip;
  ifStrip.sourceDownmixPhase = 0.0f;
  ifStrip.ifEnvelopePhase = 0.0f;
  ifStrip.detectorInputI = 0.0f;
  ifStrip.detectorInputQ = 0.0f;
  ifStrip.prevSourceMode = SourceInputMode::ComplexEnvelope;
  ifStrip.prevSourceI = 0.0f;
  ifStrip.prevSourceQ = 0.0f;
  ifStrip.sourceEnvelope.reset();
  ifStrip.loadedCanEnvelope.reset();
}

void RadioIFStripNode::setBandwidth(Radio1938& radio, float bwHz, float tuneHz) {
  auto& ifStrip = radio.ifStrip;
  auto& demod = radio.demod;
  auto resonantCapacitanceFarads = [](float freqHz, float inductanceHenries) {
    float omega = kRadioTwoPi * std::max(freqHz, 1.0f);
    return 1.0f /
           std::max(omega * omega * std::max(inductanceHenries, 1e-9f), 1e-18f);
  };
  auto seriesResistanceForBandwidth = [](float inductanceHenries,
                                         float bandwidthHz) {
    return std::max(kRadioTwoPi * std::max(inductanceHenries, 1e-9f) *
                        std::max(bandwidthHz, 1.0f),
                    1e-4f);
  };
  auto seriesBandwidthHz = [](float inductanceHenries, float resistanceOhms) {
    return std::max(resistanceOhms, 1e-4f) /
           (kRadioTwoPi * std::max(inductanceHenries, 1e-9f));
  };
  auto parallelLoadBandwidthHz = [](float capacitanceFarads,
                                    float resistanceOhms) {
    if (capacitanceFarads <= 0.0f || resistanceOhms <= 0.0f) return 0.0f;
    return 1.0f / (kRadioTwoPi * resistanceOhms * capacitanceFarads);
  };
  float sampleRate = std::max(radio.sampleRate, 1.0f);
  float safeAudioBw = std::max(bwHz, ifStrip.ifMinBwHz);
  float physicalChannelBw = 2.0f * safeAudioBw;
  ifStrip.sourceCarrierHz = selectSourceCarrierHz(
      sampleRate, sampleRate, 0.0f, physicalChannelBw);
  ifStrip.loFrequencyHz = ifStrip.sourceCarrierHz + ifStrip.ifCenterHz + tuneHz;

  float primaryDrift = std::max(radio.identity.ifPrimaryDrift, 0.65f);
  float secondaryDrift = std::max(radio.identity.ifSecondaryDrift, 0.65f);
  float couplingDrift = std::max(radio.identity.ifCouplingDrift, 0.65f);
  float primaryInductance =
      std::max(ifStrip.primaryInductanceHenries * primaryDrift, 1e-9f);
  float secondaryInductance =
      std::max(ifStrip.secondaryInductanceHenries * secondaryDrift, 1e-9f);
  float interstageCouplingCoeff =
      clampf(ifStrip.interstageCouplingCoeff * couplingDrift, 0.05f, 0.35f);
  float outputCouplingCoeff =
      clampf(ifStrip.outputCouplingCoeff * couplingDrift, 0.04f, 0.30f);
  float nominalCanBandwidthHz = std::max(physicalChannelBw * 1.10f, 1200.0f);
  float primaryCapacitanceFarads =
      resonantCapacitanceFarads(ifStrip.ifCenterHz, primaryInductance);
  float secondaryCapacitanceFarads =
      resonantCapacitanceFarads(ifStrip.ifCenterHz, secondaryInductance);
  float primarySeriesResistance =
      seriesResistanceForBandwidth(primaryInductance, nominalCanBandwidthHz);
  float secondarySeriesResistance =
      seriesResistanceForBandwidth(secondaryInductance, nominalCanBandwidthHz);
  float primaryTankBandwidthHz =
      seriesBandwidthHz(primaryInductance, primarySeriesResistance);
  float secondaryTankBandwidthHz =
      seriesBandwidthHz(secondaryInductance, secondarySeriesResistance);
  float secondaryLoadBandwidthHz = parallelLoadBandwidthHz(
      secondaryCapacitanceFarads, ifStrip.secondaryLoadResistanceOhms);
  float inductanceAsymmetry =
      std::clamp(std::sqrt(primaryInductance / secondaryInductance), 0.85f, 1.18f);
  float tankBandwidthTracking =
      std::clamp(0.5f * (primaryTankBandwidthHz + secondaryTankBandwidthHz) /
                     std::max(nominalCanBandwidthHz, 1.0f),
                 0.70f, 1.35f);
  float couplingMean = 0.5f * (interstageCouplingCoeff + outputCouplingCoeff);
  float loadSeverity =
      std::clamp(nominalCanBandwidthHz /
                     std::max(secondaryLoadBandwidthHz, nominalCanBandwidthHz),
                 0.0f, 1.0f);
  float tunedCanEnvelopeBandwidthHz =
      std::clamp(nominalCanBandwidthHz * tankBandwidthTracking *
                     (1.0f + 1.35f * couplingMean) *
                     (1.0f - 0.22f * std::fabs(inductanceAsymmetry - 1.0f)) *
                     (1.0f - 0.18f * loadSeverity),
                 std::max(1800.0f, 0.80f * safeAudioBw), 0.20f * sampleRate);
  float tunedCanEnvelopeQ =
      std::clamp(0.82f + 0.85f * couplingMean - 0.30f * loadSeverity, 0.70f,
                 1.10f);
  // This stage is only the analytic downmix image rejector. It must stay
  // substantially wider than the loaded IF-can equivalent so the IF can, not a
  // cascaded helper LP, sets the audio sideband bandwidth.
  float sourceEnvelopeLpHz =
      std::clamp(std::max(4.0f * safeAudioBw, 0.72f * ifStrip.sourceCarrierHz),
                 2.0f * tunedCanEnvelopeBandwidthHz, 0.42f * sampleRate);
  // The IF strip stays a reduced-order baseband model, but its single complex
  // transfer is still derived from the tuned-can bandwidth, coupling, loading,
  // and primary/secondary mismatch. The user-facing bwHz remains an audio
  // sideband target; the physical IF channel that feeds this baseband transfer
  // is double-sideband around the carrier.
  ifStrip.sourceEnvelope.setLowpass(sampleRate, sourceEnvelopeLpHz,
                                    kRadioBiquadQ);
  ifStrip.loadedCanEnvelope.setLowpass(sampleRate, tunedCanEnvelopeBandwidthHz,
                                       tunedCanEnvelopeQ);

  float senseLow = ifStrip.ifCenterHz - 0.5f * physicalChannelBw;
  float senseHigh = ifStrip.ifCenterHz + 0.5f * physicalChannelBw;
  demod.am.setSenseWindow(senseLow, senseHigh);
  if (demod.am.fs > 0.0f) {
    demod.am.setBandwidth(safeAudioBw, tuneHz);
  }
}

float RadioIFStripNode::process(Radio1938& radio,
                                float y,
                                const RadioSampleContext& ctx) {
  auto& frontEnd = radio.frontEnd;
  auto& mixer = radio.mixer;
  auto& ifStrip = radio.ifStrip;
  ifStrip.detectorInputI = 0.0f;
  ifStrip.detectorInputQ = 0.0f;
  if (!ifStrip.enabled || radio.sampleRate <= 0.0f) {
    return y;
  }

  SourceInputMode mode = radio.sourceFrame.mode;
  float currI =
      (mode == SourceInputMode::RealRf) ? y : radio.sourceFrame.i;
  float currQ = radio.sourceFrame.q;
  if (mode != ifStrip.prevSourceMode) {
    ifStrip.sourceEnvelope.reset();
    ifStrip.loadedCanEnvelope.reset();
    ifStrip.sourceDownmixPhase = 0.0f;
    ifStrip.ifEnvelopePhase = 0.0f;
  }
  float avcT = clampf(radio.controlBus.controlVoltage / 1.25f, 0.0f, 1.0f);
  float rfGain =
      frontEnd.rfGain * std::max(0.35f, 1.0f - frontEnd.avcGainDepth * avcT);
  float ifGainControl =
      std::max(0.20f, 1.0f - ifStrip.avcGainDepth * avcT);
  float ifGain = ifStrip.stageGain * ifGainControl;
  float sourceEnvI = 0.0f;
  float sourceEnvQ = 0.0f;
  if (mode == SourceInputMode::ComplexEnvelope) {
    sourceEnvI = rfGain * currI;
    sourceEnvQ = rfGain * currQ;
  } else {
    float sourceStep =
        kRadioTwoPi * (ifStrip.sourceCarrierHz / std::max(radio.sampleRate, 1.0f));
    float phase = ifStrip.sourceDownmixPhase;
    float c = std::cos(phase);
    float s = std::sin(phase);
    auto env = ifStrip.sourceEnvelope.process(2.0f * currI * c,
                                              -2.0f * currI * s);
    sourceEnvI = env[0];
    sourceEnvQ = env[1];
    ifStrip.sourceDownmixPhase = wrapPhase(phase + sourceStep);
  }

  float tuneHz = ifStrip.loFrequencyHz - ifStrip.sourceCarrierHz - ifStrip.ifCenterHz;
  float sampleRate = std::max(radio.sampleRate, 1.0f);
  float tuneStep = kRadioTwoPi * (tuneHz / sampleRate);
  float tunePhase = ifStrip.ifEnvelopePhase;
  auto rotatedEnv = rotateComplexEnvelope(sourceEnvI, sourceEnvQ, tunePhase);
  float rotatedEnvI = rotatedEnv[0];
  float rotatedEnvQ = rotatedEnv[1];
  ifStrip.ifEnvelopePhase = wrapPhase(tunePhase + tuneStep);

  float mixerConversionGain = mixer.conversionGain;
  assert(std::isfinite(mixerConversionGain) &&
         std::fabs(mixerConversionGain) >= 1e-6f);
  float ifEnvI = rotatedEnvI * mixerConversionGain * ifGain;
  float ifEnvQ = rotatedEnvQ * mixerConversionGain * ifGain;
  auto loadedCanEnv = ifStrip.loadedCanEnvelope.process(ifEnvI, ifEnvQ);
  auto detectorEnv =
      unrotateComplexEnvelope(loadedCanEnv[0], loadedCanEnv[1], tunePhase);
  assert(std::isfinite(detectorEnv[0]) && std::isfinite(detectorEnv[1]));
  ifStrip.detectorInputI = detectorEnv[0];
  ifStrip.detectorInputQ = detectorEnv[1];
  radio.sourceFrame.setComplexEnvelope(detectorEnv[0], detectorEnv[1]);

  if (radio.calibration.enabled) {
    FixedPlatePentodeEvaluator mixerPlateCurrentForGrid =
        prepareFixedPlatePentodeEvaluator(
            mixer.plateQuiescentVolts, mixer.screenVolts, mixer.biasVolts,
            mixer.modelCutoffVolts, mixer.plateQuiescentVolts,
            mixer.screenVolts, mixer.plateQuiescentCurrentAmps,
            mixer.mutualConductanceSiemens, mixer.plateKneeVolts,
            mixer.gridSoftnessVolts);
    float sourceEnvAbs =
        std::sqrt(sourceEnvI * sourceEnvI + sourceEnvQ * sourceEnvQ);
    float mixedCurrent = mixerPlateCurrentForGrid.eval(
        mixer.mixedBaseGridVolts + mixer.loGridDriveVolts +
        mixer.rfGridDriveVolts * radio.identity.mixerDriveDrift * sourceEnvAbs);
    radio.calibration.maxMixerPlateCurrentAmps =
        std::max(radio.calibration.maxMixerPlateCurrentAmps, mixedCurrent);
  }

  ifStrip.prevSourceMode = mode;
  ifStrip.prevSourceI = currI;
  ifStrip.prevSourceQ = currQ;
  return std::sqrt(detectorEnv[0] * detectorEnv[0] +
                   detectorEnv[1] * detectorEnv[1]);
}

void RadioDemodNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& demod = radio.demod;
  demod.am.init(radio.sampleRate, initCtx.tunedBw,
                radio.tuning.tuneOffsetHz);
}

void RadioDemodNode::reset(Radio1938& radio) { radio.demod.am.reset(); }

static float estimateMixerEnvelopeConversionGain(
    const FixedPlatePentodeEvaluator& plateCurrentForGrid,
    float mixedBaseGridVolts,
    float loGridDriveVolts) {
  constexpr int kMixerHarmonicSamples = 8;
  float baseDerivative = plateCurrentForGrid.evalDerivative(mixedBaseGridVolts);
  float harmonicSum = 0.0f;
  for (int i = 0; i < kMixerHarmonicSamples; ++i) {
    float phase =
        kRadioTwoPi * (static_cast<float>(i) + 0.5f) /
        static_cast<float>(kMixerHarmonicSamples);
    float loGridVolts = loGridDriveVolts * std::cos(phase);
    float drivenDerivative = plateCurrentForGrid.evalDerivative(
        mixedBaseGridVolts + loGridVolts);
    harmonicSum += (drivenDerivative - baseDerivative) * std::cos(phase);
  }
  return harmonicSum / static_cast<float>(kMixerHarmonicSamples);
}

static inline std::array<float, 2> rotateComplexEnvelope(float inI,
                                                         float inQ,
                                                         float phase) {
  float c = std::cos(phase);
  float s = std::sin(phase);
  return {inI * c + inQ * s, inQ * c - inI * s};
}

static inline std::array<float, 2> unrotateComplexEnvelope(float inI,
                                                           float inQ,
                                                           float phase) {
  float c = std::cos(phase);
  float s = std::sin(phase);
  return {inI * c - inQ * s, inQ * c + inI * s};
}

float RadioDemodNode::process(Radio1938& radio,
                              float y,
                              const RadioSampleContext& ctx) {
  auto& demod = radio.demod;
  if (radio.ifStrip.enabled) {
    y = demod.am.processEnvelope(radio.ifStrip.detectorInputI,
                                 radio.ifStrip.detectorInputQ,
                                 ctx.derived.demodIfNoiseAmp,
                                 radio,
                                 ctx.derived.demodIfCrackleAmp,
                                 ctx.derived.demodIfCrackleRate);
  } else if (radio.sourceFrame.mode == SourceInputMode::ComplexEnvelope) {
    y = demod.am.processEnvelope(radio.sourceFrame.i, radio.sourceFrame.q,
                                 ctx.derived.demodIfNoiseAmp,
                                 radio,
                                 ctx.derived.demodIfCrackleAmp,
                                 ctx.derived.demodIfCrackleRate);
  } else {
    y = demod.am.processEnvelope(y, 0.0f, ctx.derived.demodIfNoiseAmp,
                                 radio,
                                 ctx.derived.demodIfCrackleAmp,
                                 ctx.derived.demodIfCrackleRate);
  }
  radio.controlSense.controlVoltageSense =
      clampf(demod.am.avcEnv / std::max(demod.am.controlVoltageRef, 1e-6f),
             0.0f, 1.25f);
  radio.controlSense.tuningErrorSense = demod.am.afcError;
  return y;
}

void RadioReceiverCircuitNode::init(Radio1938& radio, RadioInitContext&) {
  auto& receiver = radio.receiverCircuit;
  if (!receiver.enabled || !receiver.tubeTriodeConnected) return;
  TriodeOperatingPoint op = solveTriodeOperatingPoint(
      receiver.tubePlateSupplyVolts, receiver.tubeLoadResistanceOhms,
      receiver.tubeBiasVolts, receiver.tubePlateDcVolts,
      receiver.tubePlateCurrentAmps, receiver.tubeMutualConductanceSiemens,
      receiver.tubeMu);
  receiver.tubeQuiescentPlateVolts = op.plateVolts;
  receiver.tubeQuiescentPlateCurrentAmps = op.plateCurrentAmps;
  receiver.tubePlateResistanceOhms = op.rpOhms;
  receiver.tubeTriodeModel = op.model;
  configureRuntimeTriodeLut(receiver.tubeTriodeLut, receiver.tubeTriodeModel,
                            receiver.tubeBiasVolts,
                            receiver.tubePlateSupplyVolts, 36.0f, 8.0f);
  assert((std::fabs(receiver.tubeQuiescentPlateVolts -
                    receiver.tubePlateDcVolts) <=
          receiver.operatingPointToleranceVolts) &&
         "6J5 first-audio operating point diverged from the preset target");
}

void RadioReceiverCircuitNode::reset(Radio1938& radio) {
  auto& receiver = radio.receiverCircuit;
  receiver.couplingCapVoltage = 0.0f;
  receiver.gridVoltage = 0.0f;
  receiver.volumeControlTapVoltage = 0.0f;
  receiver.inputNetworkDrivenFromDetector = false;
  receiver.warmStartPending = true;
  receiver.tubePlateVoltage = receiver.tubeQuiescentPlateVolts;
}

float RadioReceiverCircuitNode::process(Radio1938& radio,
                                        float y,
                                        const RadioSampleContext&) {
  auto& receiver = radio.receiverCircuit;
  if (!receiver.enabled) return y;
  float receiverSupplyScale =
      computePowerBranchSupplyScale(radio, radio.power.supplyDriveDepth);
  float dt = 1.0f / std::max(radio.sampleRate, 1.0f);
  bool inputNetworkPreSolved = receiver.inputNetworkDrivenFromDetector;
  receiver.inputNetworkDrivenFromDetector = false;
  if (!inputNetworkPreSolved && receiver.warmStartPending) {
    auto dcNodes = computeReceiverControlDcNodes(receiver, y);
    receiver.volumeControlTapVoltage = dcNodes[1];
    // On a warmed set the detector-side control network already sits at its
    // DC operating point, so precharge the coupling capacitor to the current
    // wiper potential instead of letting the first audio grid absorb a false
    // startup step from an empty capacitor.
    receiver.couplingCapVoltage = dcNodes[0];
    receiver.gridVoltage = 0.0f;
    receiver.warmStartPending = false;
  }
  if (!inputNetworkPreSolved) {
    auto solve = solveReceiverInputNetwork(receiver, dt, y);
    receiver.gridVoltage = solve.gridVoltage;
    receiver.volumeControlTapVoltage = solve.tapVoltage;
    receiver.couplingCapVoltage = solve.couplingCapVoltage;
    assert(std::isfinite(receiver.couplingCapVoltage) &&
           std::isfinite(receiver.gridVoltage) &&
           std::isfinite(receiver.volumeControlTapVoltage));
  }
  if (radio.calibration.enabled) {
    radio.calibration.receiverGridVolts.accumulate(receiver.gridVoltage);
  }
  float out = 0.0f;
  float plateCurrent = 0.0f;
  assert(receiver.tubeTriodeConnected &&
         "Receiver audio stage expects a triode model");
  out = processResistorLoadedTriodeStage(
      receiver.tubeBiasVolts + receiver.gridVoltage, receiverSupplyScale,
      receiver.tubePlateSupplyVolts, receiver.tubeQuiescentPlateVolts,
      receiver.tubeTriodeModel, &receiver.tubeTriodeLut,
      receiver.tubeLoadResistanceOhms,
      receiver.tubePlateVoltage);
  plateCurrent = static_cast<float>(
      evaluateKorenTriodePlateRuntime(receiver.tubeBiasVolts +
                                          receiver.gridVoltage,
                                      receiver.tubePlateVoltage,
                                      receiver.tubeTriodeModel,
                                      receiver.tubeTriodeLut)
          .currentAmps);
  if (radio.calibration.enabled) {
    radio.calibration.receiverPlateSwingVolts.accumulate(out);
    radio.calibration.maxReceiverPlateCurrentAmps =
        std::max(radio.calibration.maxReceiverPlateCurrentAmps, plateCurrent);
  }
  return out;
}

void RadioToneNode::init(Radio1938& radio, RadioInitContext&) {
  auto& tone = radio.tone;
  tone.presence.setPeaking(radio.sampleRate, tone.presenceHz, tone.presenceQ,
                           tone.presenceGainDb);
  tone.tiltLp.setLowpass(radio.sampleRate, tone.tiltSplitHz, kRadioBiquadQ);
}

void RadioToneNode::reset(Radio1938& radio) {
  auto& tone = radio.tone;
  tone.presence.reset();
  tone.tiltLp.reset();
}

float RadioToneNode::process(Radio1938& radio,
                             float y,
                             const RadioSampleContext&) {
  auto& tone = radio.tone;
  if (tone.presenceHz <= 0.0f) return y;
  return tone.presence.process(y);
}

void RadioPowerNode::init(Radio1938& radio, RadioInitContext&) {
  auto& power = radio.power;
  power.tubePlateDcVolts =
      power.tubePlateSupplyVolts -
      power.tubePlateCurrentAmps * power.interstagePrimaryResistanceOhms;
  power.outputTubePlateDcVolts =
      power.outputTubePlateSupplyVolts -
      power.outputTubePlateCurrentAmps *
          (0.5f * power.outputTransformerPrimaryResistanceOhms);
  assert(std::fabs(power.tubePlateSupplyVolts -
                   power.tubePlateCurrentAmps *
                       power.interstagePrimaryResistanceOhms -
                   power.tubePlateDcVolts) < 1.0f);
  assert(std::fabs(power.outputTubePlateSupplyVolts -
                   power.outputTubePlateCurrentAmps *
                       (0.5f * power.outputTransformerPrimaryResistanceOhms) -
                   power.outputTubePlateDcVolts) < 1.0f);
  power.sagAtk =
      std::exp(-1.0f / (radio.sampleRate * (power.sagAttackMs / 1000.0f)));
  power.sagRel =
      std::exp(-1.0f / (radio.sampleRate * (power.sagReleaseMs / 1000.0f)));
  if (power.postLpHz > 0.0f) {
    power.postLpf.setLowpass(radio.sampleRate, power.postLpHz, kRadioBiquadQ);
  } else {
    power.postLpf = Biquad{};
  }
  power.driverSourceResistanceOhms = parallelResistance(
      radio.receiverCircuit.tubeLoadResistanceOhms,
      radio.receiverCircuit.tubePlateResistanceOhms);
  assert(std::isfinite(power.driverSourceResistanceOhms) &&
         power.driverSourceResistanceOhms > 0.0f);
  TriodeOperatingPoint driverOp = solveTriodeOperatingPoint(
      power.tubePlateSupplyVolts, power.interstagePrimaryResistanceOhms,
      power.tubeBiasVolts, power.tubePlateDcVolts, power.tubePlateCurrentAmps,
      power.tubeMutualConductanceSiemens, power.tubeMu);
  power.tubeQuiescentPlateVolts = driverOp.plateVolts;
  power.tubeQuiescentPlateCurrentAmps = driverOp.plateCurrentAmps;
  power.tubePlateResistanceOhms = driverOp.rpOhms;
  power.tubeTriodeModel = driverOp.model;
  configureRuntimeTriodeLut(power.tubeTriodeLut, power.tubeTriodeModel,
                            power.tubeBiasVolts, power.tubePlateSupplyVolts,
                            96.0f, 18.0f);
  assert((std::fabs(power.tubeQuiescentPlateVolts - power.tubePlateDcVolts) <=
          power.operatingPointToleranceVolts) &&
         "6F6 driver operating point diverged from the preset target");

  TriodeOperatingPoint outputOp = solveTriodeOperatingPoint(
      power.outputTubePlateSupplyVolts,
      0.5f * power.outputTransformerPrimaryResistanceOhms,
      power.outputTubeBiasVolts, power.outputTubePlateDcVolts,
      power.outputTubePlateCurrentAmps,
      power.outputTubeMutualConductanceSiemens, power.outputTubeMu);
  power.outputTubeQuiescentPlateVolts = outputOp.plateVolts;
  power.outputTubeQuiescentPlateCurrentAmps = outputOp.plateCurrentAmps;
  power.outputTubePlateResistanceOhms = outputOp.rpOhms;
  power.outputTubeTriodeModel = outputOp.model;
  configureRuntimeTriodeLut(power.outputTubeTriodeLut,
                            power.outputTubeTriodeModel,
                            power.outputTubeBiasVolts,
                            power.outputTubePlateSupplyVolts, 120.0f, 24.0f);
  assert((std::fabs(power.outputTubeQuiescentPlateVolts -
                    power.outputTubePlateDcVolts) <=
          power.outputOperatingPointToleranceVolts) &&
         "6B4 output operating point diverged from the preset target");

  power.tubePlateVoltage = power.tubeQuiescentPlateVolts;
  power.outputGridAVolts = 0.0f;
  power.outputGridBVolts = 0.0f;
  power.interstageTransformer.configure(
      radio.sampleRate, power.interstagePrimaryLeakageInductanceHenries,
      power.interstageMagnetizingInductanceHenries,
      power.interstageTurnsRatioPrimaryToSecondary,
      power.interstagePrimaryResistanceOhms,
      power.interstagePrimaryCoreLossResistanceOhms,
      power.interstagePrimaryShuntCapFarads,
      power.interstageSecondaryLeakageInductanceHenries,
      power.interstageSecondaryResistanceOhms,
      power.interstageSecondaryShuntCapFarads,
      power.interstageIntegrationSubsteps);
  power.outputTransformer.configure(
      radio.sampleRate, power.outputTransformerPrimaryLeakageInductanceHenries,
      power.outputTransformerMagnetizingInductanceHenries,
      power.outputTransformerTurnsRatioPrimaryToSecondary,
      power.outputTransformerPrimaryResistanceOhms,
      power.outputTransformerPrimaryCoreLossResistanceOhms,
      power.outputTransformerPrimaryShuntCapFarads,
      power.outputTransformerSecondaryLeakageInductanceHenries,
      power.outputTransformerSecondaryResistanceOhms,
      power.outputTransformerSecondaryShuntCapFarads,
      power.outputTransformerIntegrationSubsteps);
  {
    FixedLoadAffineTransformerProjection outputAffine =
        buildFixedLoadAffineProjection(power.outputTransformer,
                                       power.outputLoadResistanceOhms, 0.0f);
    power.outputTransformerAffineReady = true;
    power.outputTransformerAffineStateA = outputAffine.stateA;
    power.outputTransformerAffineSlope = outputAffine.slope;
  }
  // Derive the digital speaker reference from the modeled 6B4/output-transformer
  // combination instead of a hand-tuned watt scalar. This keeps the digital
  // scale anchored to the clean power the current tube/load model can actually
  // produce.
  power.nominalOutputPowerWatts = estimateOutputStageNominalPowerWatts(power);
  assert(std::isfinite(power.nominalOutputPowerWatts) &&
         power.nominalOutputPowerWatts > 0.0f);
  radio.output.digitalReferenceSpeakerVoltsPeak = std::sqrt(
      2.0f * power.nominalOutputPowerWatts * power.outputLoadResistanceOhms);
}

void RadioPowerNode::reset(Radio1938& radio) {
  auto& power = radio.power;
  power.sagEnv = 0.0f;
  power.rectifierPhase = 0.0f;
  power.gridCouplingCapVoltage = 0.0f;
  power.gridVoltage = 0.0f;
  power.tubePlateVoltage = power.tubeQuiescentPlateVolts;
  power.outputGridAVolts = 0.0f;
  power.outputGridBVolts = 0.0f;
  power.interstageCt.primaryCurrent = 0.0f;
  power.interstageCt.primaryVoltage = 0.0f;
  power.interstageCt.secondaryACurrent = 0.0f;
  power.interstageCt.secondaryAVoltage = 0.0f;
  power.interstageCt.secondaryBCurrent = 0.0f;
  power.interstageCt.secondaryBVoltage = 0.0f;
  power.outputTransformer.reset();
  power.postLpf.reset();
}

float RadioPowerNode::process(Radio1938& radio,
                              float y,
                              const RadioSampleContext&) {
  auto& power = radio.power;
  auto& controlSense = radio.controlSense;
  if (radio.calibration.enabled) {
    radio.calibration.validationSampleCount++;
  }
  float powerT = computePowerLoadT(power);
  float driverSupplyScale =
      computePowerBranchSupplyScale(radio, power.supplyDriveDepth);
  float outputSupplyScale = computePowerBranchSupplyScale(radio, 1.0f);
  float dt = 1.0f / requirePositiveFinite(radio.sampleRate);
  float previousCapVoltage = power.gridCouplingCapVoltage;
  power.gridVoltage = solveCapCoupledGridVoltage(
      y, previousCapVoltage, dt, power.gridCouplingCapFarads,
      power.driverSourceResistanceOhms, power.gridLeakResistanceOhms,
      power.tubeBiasVolts, power.tubeGridCurrentResistanceOhms);
  float seriesCurrent =
      (y - previousCapVoltage - power.gridVoltage) /
      (power.driverSourceResistanceOhms +
       dt / requirePositiveFinite(power.gridCouplingCapFarads));
  power.gridCouplingCapVoltage +=
      dt * (seriesCurrent / requirePositiveFinite(power.gridCouplingCapFarads));
  float controlGridVolts = power.tubeBiasVolts + power.gridVoltage;
  if (radio.calibration.enabled) {
    radio.calibration.driverGridVolts.accumulate(power.gridVoltage);
  }
  if (radio.calibration.enabled && controlGridVolts > 0.0f) {
    radio.calibration.driverGridPositiveSamples++;
  }
  assert(power.tubeTriodeConnected &&
         "Interstage driver solve requires triode-connected 6F6 operation");
  float driverPlateQuiescent =
      requirePositiveFinite(power.tubeQuiescentPlateVolts * driverSupplyScale);
  float driverQuiescentCurrent = static_cast<float>(
      evaluateKorenTriodePlateRuntime(power.tubeBiasVolts,
                                      driverPlateQuiescent,
                                      power.tubeTriodeModel,
                                      power.tubeTriodeLut)
          .currentAmps);
  auto interstageSolved = solveDriverInterstageCenterTappedNoCap(
      power.interstageTransformer, power, controlGridVolts,
      driverPlateQuiescent, driverQuiescentCurrent);

  power.interstageCt.primaryCurrent = interstageSolved.primaryCurrent;
  power.interstageCt.primaryVoltage = interstageSolved.primaryVoltage;
  power.interstageCt.secondaryACurrent = interstageSolved.secondaryACurrent;
  power.interstageCt.secondaryAVoltage = interstageSolved.secondaryAVoltage;
  power.interstageCt.secondaryBCurrent = interstageSolved.secondaryBCurrent;
  power.interstageCt.secondaryBVoltage = interstageSolved.secondaryBVoltage;

  power.tubePlateVoltage =
      driverPlateQuiescent - interstageSolved.primaryVoltage;

  power.outputGridAVolts = interstageSolved.secondaryAVoltage;
  power.outputGridBVolts = interstageSolved.secondaryBVoltage;

  float actualDriverCurrent = interstageSolved.driverPlateCurrentAbs;

  if (radio.calibration.enabled) {
    radio.calibration.driverPlateSwingVolts.accumulate(
        interstageSolved.primaryVoltage);
    radio.calibration.outputGridAVolts.accumulate(power.outputGridAVolts);
    radio.calibration.outputGridBVolts.accumulate(power.outputGridBVolts);
    float interstageDifferentialVolts =
        interstageSolved.secondaryAVoltage - interstageSolved.secondaryBVoltage;

    radio.calibration.interstageSecondaryPeakVolts =
        std::max(radio.calibration.interstageSecondaryPeakVolts,
                 std::fabs(interstageDifferentialVolts));

    radio.calibration.interstageSecondarySumSq +=
        static_cast<double>(interstageDifferentialVolts) *
        static_cast<double>(interstageDifferentialVolts);
  }
  if (radio.calibration.enabled) {
    bool outputGridAPositive =
        (power.outputTubeBiasVolts + power.outputGridAVolts) > 0.0f;
    bool outputGridBPositive =
        (power.outputTubeBiasVolts + power.outputGridBVolts) > 0.0f;
    if (outputGridAPositive) radio.calibration.outputGridAPositiveSamples++;
    if (outputGridBPositive) radio.calibration.outputGridBPositiveSamples++;
    if (outputGridAPositive || outputGridBPositive) {
      radio.calibration.outputGridPositiveSamples++;
    }
  }
  float outputPlateQuiescent =
      requirePositiveFinite(power.outputTubeQuiescentPlateVolts *
                            outputSupplyScale);
  const float outputPrimaryLoadResistance = 0.0f;
  AffineTransformerProjection affineOut{};
  if (power.outputTransformerAffineReady) {
    affineOut.base = evalFixedLoadAffineBase(
        power.outputTransformerAffineStateA, power.outputTransformer);
    affineOut.slope = power.outputTransformerAffineSlope;
  } else {
    affineOut = buildAffineProjection(
        power.outputTransformer, power.outputLoadResistanceOhms,
        outputPrimaryLoadResistance);
  }
  float solvedOutputPrimaryVoltage = solveOutputPrimaryVoltageAffine(
      affineOut, power, outputPlateQuiescent, power.outputGridAVolts,
      power.outputGridBVolts, power.outputTransformer.primaryVoltage);
  float outputPlateA =
      outputPlateQuiescent - 0.5f * solvedOutputPrimaryVoltage;
  float outputPlateB =
      outputPlateQuiescent + 0.5f * solvedOutputPrimaryVoltage;
  KorenTriodePlateEval outputEvalA = evaluateKorenTriodePlateRuntime(
      power.outputTubeBiasVolts + power.outputGridAVolts, outputPlateA,
      power.outputTubeTriodeModel, power.outputTubeTriodeLut);
  KorenTriodePlateEval outputEvalB = evaluateKorenTriodePlateRuntime(
      power.outputTubeBiasVolts + power.outputGridBVolts, outputPlateB,
      power.outputTubeTriodeModel, power.outputTubeTriodeLut);
  float plateCurrentA = static_cast<float>(outputEvalA.currentAmps);
  float plateCurrentB = static_cast<float>(outputEvalB.currentAmps);
  float driveCurrent = 0.5f * (plateCurrentA - plateCurrentB);
  auto outputSample = evalAffineProjection(affineOut, driveCurrent);
  power.outputTransformer.primaryVoltage = outputSample.primaryVoltage;
  power.outputTransformer.secondaryVoltage = outputSample.secondaryVoltage;
  power.outputTransformer.primaryCurrent = outputSample.primaryCurrent;
  power.outputTransformer.secondaryCurrent = outputSample.secondaryCurrent;
  float actualOutputPlateA =
      outputPlateQuiescent - 0.5f * outputSample.primaryVoltage;
  float actualOutputPlateB =
      outputPlateQuiescent + 0.5f * outputSample.primaryVoltage;
  float actualPlateCurrentA = static_cast<float>(
      evaluateKorenTriodePlateRuntime(
          power.outputTubeBiasVolts + power.outputGridAVolts, actualOutputPlateA,
          power.outputTubeTriodeModel, power.outputTubeTriodeLut)
          .currentAmps);
  float actualPlateCurrentB = static_cast<float>(
      evaluateKorenTriodePlateRuntime(
          power.outputTubeBiasVolts + power.outputGridBVolts, actualOutputPlateB,
          power.outputTubeTriodeModel, power.outputTubeTriodeLut)
          .currentAmps);
  y = outputSample.secondaryVoltage;
  if (power.postLpHz > 0.0f) {
    y = power.postLpf.process(y);
  }
  if (radio.calibration.enabled) {
    radio.calibration.outputPrimaryVolts.accumulate(outputSample.primaryVoltage);
    radio.calibration.speakerSecondaryVolts.accumulate(y);
    radio.calibration.maxDriverPlateCurrentAmps = std::max(
        radio.calibration.maxDriverPlateCurrentAmps, actualDriverCurrent);
    radio.calibration.maxOutputPlateCurrentAAmps = std::max(
        radio.calibration.maxOutputPlateCurrentAAmps, actualPlateCurrentA);
    radio.calibration.maxOutputPlateCurrentBAmps = std::max(
        radio.calibration.maxOutputPlateCurrentBAmps, actualPlateCurrentB);
    radio.calibration.maxSpeakerSecondaryVolts =
        std::max(radio.calibration.maxSpeakerSecondaryVolts, std::fabs(y));
    radio.calibration.maxSpeakerReferenceRatio = std::max(
        radio.calibration.maxSpeakerReferenceRatio,
        std::fabs(y) /
            std::max(radio.output.digitalReferenceSpeakerVoltsPeak, 1e-3f));
  }
  float quiescentSupplyCurrent =
      driverQuiescentCurrent + 2.0f * power.outputTubeQuiescentPlateCurrentAmps;
  float actualSupplyCurrent =
      actualDriverCurrent + actualPlateCurrentA + actualPlateCurrentB;
  float load = std::max(
      0.0f, (actualSupplyCurrent - quiescentSupplyCurrent) /
                requirePositiveFinite(quiescentSupplyCurrent));
  if (load > power.sagEnv) {
    power.sagEnv = power.sagAtk * power.sagEnv + (1.0f - power.sagAtk) * load;
  } else {
    power.sagEnv = power.sagRel * power.sagEnv + (1.0f - power.sagRel) * load;
  }
  controlSense.powerSagSense = power.sagEnv;
  advanceRectifierRipplePhase(radio);
  return y;
}

void RadioInterferenceDerivedNode::init(Radio1938& radio, RadioInitContext&) {
  auto& noiseConfig = radio.noiseConfig;
  auto& noiseDerived = radio.noiseDerived;
  if (radio.noiseWeight <= 0.0f) {
    noiseDerived.baseNoiseAmp = 0.0f;
    noiseDerived.baseCrackleAmp = 0.0f;
    noiseDerived.baseHumAmp = 0.0f;
    noiseDerived.crackleRate = 0.0f;
    return;
  }

  float scale =
      radio.noiseWeight / std::max(noiseConfig.noiseWeightRef, 1e-6f);
  if (noiseConfig.noiseWeightScaleMax > 0.0f) {
    scale = std::min(scale, noiseConfig.noiseWeightScaleMax);
  }
  noiseDerived.baseNoiseAmp = radio.noiseWeight;
  noiseDerived.baseCrackleAmp = noiseConfig.crackleAmpScale * scale;
  noiseDerived.baseHumAmp = noiseConfig.humAmpScale * scale;
  noiseDerived.crackleRate = noiseConfig.crackleRateScale * scale;
}

void RadioInterferenceDerivedNode::reset(Radio1938&) {}

void RadioInterferenceDerivedNode::update(Radio1938& radio,
                                          RadioSampleContext& ctx) {
  auto& noiseDerived = radio.noiseDerived;
  float avcT = clampf(radio.controlBus.controlVoltage / 1.25f, 0.0f, 1.0f);
  float rfGainControl = std::max(
      0.35f, 1.0f - radio.frontEnd.avcGainDepth * avcT);
  float ifGainControl = std::max(
      0.20f, 1.0f - radio.ifStrip.avcGainDepth * avcT);
  float preDetectorGain = rfGainControl * ifGainControl;
  float mistuneT = 0.0f;
  if (ctx.block) {
    mistuneT = clampf(std::fabs(ctx.block->tuneNorm), 0.0f, 1.0f);
  } else {
    float bwHalf = 0.5f * std::max(radio.tuning.tunedBw, 1.0f);
    mistuneT = clampf(std::fabs(radio.tuning.tuneAppliedHz) /
                          std::max(bwHalf, 1e-6f),
                      0.0f, 1.0f);
  }
  float detectorLockT =
      1.0f - clampf(std::fabs(radio.controlSense.tuningErrorSense),
                    0.0f, 1.0f);
  float tunedCaptureT = 1.0f - mistuneT;
  float carrierT = clampf(radio.controlBus.controlVoltage, 0.0f, 1.0f);
  float crackleExposure = 1.0f - clampf(carrierT * tunedCaptureT * detectorLockT,
                                        0.0f, 1.0f);
  float crackleExposureSq = crackleExposure * crackleExposure;

  // RF/IF interference follows the same AVC-governed gain as the incoming
  // carrier. Impulsive bursts are most audible when carrier capture is weak,
  // either because the station is weak or because the tuned passband is
  // offset enough that the detector sees an asymmetric envelope.
  ctx.derived.demodIfNoiseAmp =
      noiseDerived.baseNoiseAmp * radio.globals.ifNoiseMix * preDetectorGain;
  bool crackleAtDetector =
      radio.graph.isEnabled(StageId::IFStrip) &&
      radio.graph.isEnabled(StageId::Demod);
  if (crackleAtDetector) {
    ctx.derived.demodIfCrackleAmp =
        noiseDerived.baseCrackleAmp * preDetectorGain * crackleExposureSq;
    ctx.derived.demodIfCrackleRate =
        noiseDerived.crackleRate * crackleExposure;
    ctx.derived.crackleAmp = 0.0f;
    ctx.derived.crackleRate = 0.0f;
  } else {
    ctx.derived.demodIfCrackleAmp = 0.0f;
    ctx.derived.demodIfCrackleRate = 0.0f;
    ctx.derived.crackleAmp =
        noiseDerived.baseCrackleAmp * crackleExposureSq;
    ctx.derived.crackleRate =
        noiseDerived.crackleRate * crackleExposure;
  }
  ctx.derived.noiseAmp =
      noiseDerived.baseNoiseAmp * radio.globals.postNoiseMix;
  // Mains hum is modeled through power-supply ripple in the receiver/power
  // stages, not as a post-speaker tone injector.
  ctx.derived.humAmp = 0.0f;
  ctx.derived.humToneEnabled = false;
}

void RadioNoiseNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& noiseConfig = radio.noiseConfig;
  auto& noiseRuntime = radio.noiseRuntime;
  noiseRuntime.hum.setFs(radio.sampleRate, initCtx.tunedBw);
  noiseRuntime.hum.humHz = noiseConfig.humHzDefault;
}

void RadioNoiseNode::reset(Radio1938& radio) { radio.noiseRuntime.hum.reset(); }

float RadioNoiseNode::process(Radio1938& radio,
                              float y,
                              const RadioSampleContext& ctx) {
  NoiseInput noiseIn{};
  noiseIn.programSample = y;
  noiseIn.noiseAmp = ctx.derived.noiseAmp;
  noiseIn.crackleAmp = ctx.derived.crackleAmp;
  noiseIn.crackleRate = ctx.derived.crackleRate;
  noiseIn.humAmp = ctx.derived.humAmp;
  noiseIn.humToneEnabled = ctx.derived.humToneEnabled;
  return y + radio.noiseRuntime.hum.process(noiseIn);
}

void RadioSpeakerNode::init(Radio1938& radio, RadioInitContext&) {
  auto& speakerStage = radio.speakerStage;
  float osFs = radio.sampleRate * radio.globals.oversampleFactor;
  float osCut = radio.sampleRate * radio.globals.oversampleCutoffFraction;
  speakerStage.osLpIn.setLowpass(osFs, osCut, kRadioBiquadQ);
  speakerStage.osLpOut.setLowpass(osFs, osCut, kRadioBiquadQ);
  speakerStage.speaker.init(osFs);
  speakerStage.speaker.drive = speakerStage.drive;
}

void RadioSpeakerNode::reset(Radio1938& radio) {
  auto& speakerStage = radio.speakerStage;
  speakerStage.osPrev = 0.0f;
  speakerStage.osLpIn.reset();
  speakerStage.osLpOut.reset();
  speakerStage.speaker.reset();
}

float RadioSpeakerNode::process(Radio1938& radio,
                                float y,
                                const RadioSampleContext&) {
  auto& speakerStage = radio.speakerStage;
  speakerStage.speaker.drive = std::max(speakerStage.drive, 0.0f);
  y = processOversampled2x(y, speakerStage.osPrev, speakerStage.osLpIn,
                           speakerStage.osLpOut, [&](float v) {
                             bool clipped = false;
                             float out = speakerStage.speaker.process(v, clipped);
                             if (clipped) radio.diagnostics.markSpeakerClip();
                             return out;
                           });
  return y;
}

void RadioCabinetNode::init(Radio1938& radio, RadioInitContext&) {
  auto& cabinet = radio.cabinet;
  float panelHzDerived =
      cabinet.panelHz * (1.0f + 0.52f * cabinet.panelStiffnessTolerance);
  float chassisHzDerived =
      cabinet.chassisHz * (1.0f + 0.28f * cabinet.panelStiffnessTolerance -
                           0.18f * cabinet.baffleLeakTolerance);
  float cavityDipHzDerived =
      cabinet.cavityDipHz * (1.0f + 0.35f * cabinet.cavityTolerance);
  float rearDelayMsDerived =
      cabinet.rearDelayMs *
      (1.0f + 0.30f * cabinet.rearPathTolerance +
       0.12f * cabinet.baffleLeakTolerance);
  cabinet.rearMixApplied =
      cabinet.rearMix * (1.0f + 0.42f * cabinet.baffleLeakTolerance -
                         0.25f * cabinet.grilleClothTolerance);

  if (cabinet.panelHz > 0.0f && cabinet.panelQ > 0.0f) {
    cabinet.panel.setPeaking(radio.sampleRate, panelHzDerived, cabinet.panelQ,
                             cabinet.panelGainDb);
  } else {
    cabinet.panel = Biquad{};
  }
  if (cabinet.chassisHz > 0.0f && cabinet.chassisQ > 0.0f) {
    cabinet.chassis.setPeaking(radio.sampleRate, chassisHzDerived,
                               cabinet.chassisQ, cabinet.chassisGainDb);
  } else {
    cabinet.chassis = Biquad{};
  }
  if (cabinet.cavityDipHz > 0.0f && cabinet.cavityDipQ > 0.0f) {
    cabinet.cavityDip.setPeaking(radio.sampleRate, cavityDipHzDerived,
                                 cabinet.cavityDipQ, cabinet.cavityDipGainDb);
  } else {
    cabinet.cavityDip = Biquad{};
  }
  if (cabinet.grilleLpHz > 0.0f) {
    float grilleLpHzDerived =
        cabinet.grilleLpHz / (1.0f + 0.55f * cabinet.grilleClothTolerance);
    cabinet.grilleLp.setLowpass(radio.sampleRate, grilleLpHzDerived,
                                kRadioBiquadQ);
  } else {
    cabinet.grilleLp = Biquad{};
  }
  if (cabinet.rearMixApplied > 0.0f) {
    if (cabinet.rearHpHz > 0.0f) {
      cabinet.rearHp.setHighpass(radio.sampleRate, cabinet.rearHpHz,
                                 kRadioBiquadQ);
    } else {
      cabinet.rearHp = Biquad{};
    }
    if (cabinet.rearLpHz > 0.0f) {
      cabinet.rearLp.setLowpass(radio.sampleRate, cabinet.rearLpHz,
                                kRadioBiquadQ);
    } else {
      cabinet.rearLp = Biquad{};
    }
    int rearSamples =
        static_cast<int>(std::ceil(rearDelayMsDerived * 0.001f *
                                   radio.sampleRate)) +
        cabinet.bufferGuardSamples;
    if (rearSamples > 0) {
      cabinet.buf.assign(static_cast<size_t>(rearSamples), 0.0f);
    } else {
      cabinet.buf.clear();
    }
  } else {
    cabinet.rearHp = Biquad{};
    cabinet.rearLp = Biquad{};
    cabinet.buf.clear();
  }
  cabinet.clarifier1 = Biquad{};
  cabinet.clarifier2 = Biquad{};
  cabinet.clarifier3 = Biquad{};
  cabinet.index = 0;
}

void RadioCabinetNode::reset(Radio1938& radio) {
  auto& cabinet = radio.cabinet;
  cabinet.panel.reset();
  cabinet.chassis.reset();
  cabinet.cavityDip.reset();
  cabinet.grilleLp.reset();
  cabinet.rearHp.reset();
  cabinet.rearLp.reset();
  cabinet.clarifier1.reset();
  cabinet.clarifier2.reset();
  cabinet.clarifier3.reset();
  cabinet.index = 0;
  std::fill(cabinet.buf.begin(), cabinet.buf.end(), 0.0f);
}

float RadioCabinetNode::process(Radio1938& radio,
                                float y,
                                const RadioSampleContext&) {
  auto& cabinet = radio.cabinet;
  if (!cabinet.enabled) return y;

  float out = cabinet.panel.process(y);
  out = cabinet.chassis.process(out);
  out = cabinet.cavityDip.process(out);
  if (!cabinet.buf.empty()) {
    float rear = cabinet.buf[static_cast<size_t>(cabinet.index)];
    cabinet.buf[static_cast<size_t>(cabinet.index)] = y;
    cabinet.index = (cabinet.index + 1) % static_cast<int>(cabinet.buf.size());
    rear = cabinet.rearHp.process(rear);
    rear = cabinet.rearLp.process(rear);
    out -= rear * cabinet.rearMixApplied;
  }
  if (cabinet.grilleLpHz > 0.0f) {
    out = cabinet.grilleLp.process(out);
  }
  return out;
}

void RadioFinalLimiterNode::init(Radio1938& radio, RadioInitContext&) {
  auto& limiter = radio.finalLimiter;
  limiter.attackCoeff =
      std::exp(-1.0f / (radio.sampleRate * (limiter.attackMs / 1000.0f)));
  limiter.releaseCoeff =
      std::exp(-1.0f / (radio.sampleRate * (limiter.releaseMs / 1000.0f)));
  limiter.delaySamples =
      static_cast<int>(std::lround(radio.sampleRate *
                                   (limiter.lookaheadMs / 1000.0f)));
  limiter.delayBuf.assign(static_cast<size_t>(limiter.delaySamples), 0.0f);
  limiter.requiredGainBuf.assign(static_cast<size_t>(limiter.delaySamples), 1.0f);
  limiter.delayWriteIndex = 0;
  float osFs = radio.sampleRate * radio.globals.oversampleFactor;
  float osCut = radio.sampleRate * radio.globals.oversampleCutoffFraction;
  limiter.osLpIn.setLowpass(osFs, osCut, kRadioBiquadQ);
  limiter.osLpOut.setLowpass(osFs, osCut, kRadioBiquadQ);
}

void RadioFinalLimiterNode::reset(Radio1938& radio) {
  auto& limiter = radio.finalLimiter;
  limiter.gain = 1.0f;
  limiter.targetGain = 1.0f;
  limiter.osPrev = 0.0f;
  limiter.observedPeak = 0.0f;
  limiter.delayWriteIndex = 0;
  std::fill(limiter.delayBuf.begin(), limiter.delayBuf.end(), 0.0f);
  std::fill(limiter.requiredGainBuf.begin(), limiter.requiredGainBuf.end(), 1.0f);
  limiter.osLpIn.reset();
  limiter.osLpOut.reset();
}

float RadioFinalLimiterNode::process(Radio1938& radio,
                                     float y,
                                     const RadioSampleContext&) {
  auto& limiter = radio.finalLimiter;
  if (!limiter.enabled) return y;
  float limitedIn = y;

  float peak = 0.0f;
  float mid = 0.5f * (limiter.osPrev + limitedIn);
  float s0 = limiter.osLpIn.process(mid);
  s0 = limiter.osLpOut.process(s0);
  peak = std::max(peak, std::fabs(s0));

  float s1 = limiter.osLpIn.process(limitedIn);
  s1 = limiter.osLpOut.process(s1);
  peak = std::max(peak, std::fabs(s1));

  limiter.osPrev = limitedIn;
  limiter.observedPeak = peak;

  float requiredGain = 1.0f;
  if (peak > limiter.threshold && peak > 1e-9f) {
    requiredGain = limiter.threshold / peak;
  }

  float delayed = limitedIn;
  if (!limiter.delayBuf.empty()) {
    size_t writeIndex = static_cast<size_t>(limiter.delayWriteIndex);
    delayed = limiter.delayBuf[writeIndex];
    limiter.delayBuf[writeIndex] = limitedIn;
    limiter.requiredGainBuf[writeIndex] = requiredGain;
    limiter.delayWriteIndex =
        (limiter.delayWriteIndex + 1) % static_cast<int>(limiter.delayBuf.size());
    limiter.targetGain = 1.0f;
    for (float gainCandidate : limiter.requiredGainBuf) {
      limiter.targetGain = std::min(limiter.targetGain, gainCandidate);
    }
  } else {
    limiter.targetGain = requiredGain;
  }

  if (limiter.targetGain < limiter.gain) {
    limiter.gain = limiter.targetGain;
  } else {
    limiter.gain =
        limiter.releaseCoeff * limiter.gain +
        (1.0f - limiter.releaseCoeff) * limiter.targetGain;
  }

  float out = delayed * limiter.gain;
  float gainReduction = 1.0f - limiter.gain;
  float gainReductionDb = (limiter.gain < 0.999999f) ? -lin2db(limiter.gain) : 0.0f;

  radio.diagnostics.finalLimiterPeak =
      std::max(radio.diagnostics.finalLimiterPeak, peak);
  radio.diagnostics.finalLimiterGain =
      std::min(radio.diagnostics.finalLimiterGain, limiter.gain);
  radio.diagnostics.finalLimiterMaxGainReduction =
      std::max(radio.diagnostics.finalLimiterMaxGainReduction, gainReduction);
  radio.diagnostics.finalLimiterMaxGainReductionDb =
      std::max(radio.diagnostics.finalLimiterMaxGainReductionDb, gainReductionDb);
  radio.diagnostics.finalLimiterGainReductionSum += gainReduction;
  radio.diagnostics.finalLimiterGainReductionDbSum += gainReductionDb;
  radio.diagnostics.processedSamples++;
  if (limiter.gain < 0.999f) {
    radio.diagnostics.finalLimiterActive = true;
    radio.diagnostics.finalLimiterActiveSamples++;
  }

  if (radio.calibration.enabled) {
    if (std::fabs(limitedIn) > radio.globals.outputClipThreshold) {
      radio.calibration.preLimiterClipCount++;
    }
    if (std::fabs(out) > radio.globals.outputClipThreshold) {
      radio.calibration.postLimiterClipCount++;
    }
    radio.calibration.totalSamples++;
    radio.calibration.limiterGainReductionSum += gainReduction;
    radio.calibration.limiterGainReductionDbSum += gainReductionDb;
    if (limiter.gain < 0.999f) {
      radio.calibration.limiterActiveSamples++;
    }
    radio.calibration.limiterMaxGainReduction =
        std::max(radio.calibration.limiterMaxGainReduction, gainReduction);
    radio.calibration.limiterMaxGainReductionDb =
        std::max(radio.calibration.limiterMaxGainReductionDb, gainReductionDb);
  }

  return out;
}

void RadioOutputClipNode::init(Radio1938& radio, RadioInitContext&) {
  auto& output = radio.output;
  float osFs = radio.sampleRate * radio.globals.oversampleFactor;
  float osCut = radio.sampleRate * radio.globals.oversampleCutoffFraction;
  output.clipOsLpIn.setLowpass(osFs, osCut, kRadioBiquadQ);
  output.clipOsLpOut.setLowpass(osFs, osCut, kRadioBiquadQ);
}

void RadioOutputClipNode::reset(Radio1938& radio) {
  auto& output = radio.output;
  output.clipOsPrev = 0.0f;
  output.clipOsLpIn.reset();
  output.clipOsLpOut.reset();
}

float RadioOutputClipNode::process(Radio1938& radio,
                                   float y,
                                   const RadioSampleContext&) {
  auto& output = radio.output;
  return processOversampled2x(y, output.clipOsPrev, output.clipOsLpIn,
                              output.clipOsLpOut, [&](float v) {
                                float t = radio.globals.outputClipThreshold;
                                float av = std::fabs(v);
                                if (av > t) radio.diagnostics.markOutputClip();
                                float clipped = softClip(v, t);
                                return std::clamp(clipped, -t, t);
                              });
}

Radio1938::BlockStep* Radio1938::RadioExecutionGraph::findBlock(StageId id) {
  for (auto& step : blockSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

const Radio1938::BlockStep* Radio1938::RadioExecutionGraph::findBlock(
    StageId id) const {
  for (const auto& step : blockSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

Radio1938::SampleControlStep* Radio1938::RadioExecutionGraph::findSampleControl(
    StageId id) {
  for (auto& step : sampleControlSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

const Radio1938::SampleControlStep*
Radio1938::RadioExecutionGraph::findSampleControl(StageId id) const {
  for (const auto& step : sampleControlSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

Radio1938::ProgramPathStep* Radio1938::RadioExecutionGraph::findProgramPath(
    StageId id) {
  for (auto& step : programPathSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

const Radio1938::ProgramPathStep* Radio1938::RadioExecutionGraph::findProgramPath(
    StageId id) const {
  for (const auto& step : programPathSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

bool Radio1938::RadioExecutionGraph::isEnabled(StageId id) const {
  if (const auto* step = findBlock(id)) return step->enabled;
  if (const auto* step = findSampleControl(id)) return step->enabled;
  if (const auto* step = findProgramPath(id)) return step->enabled;
  return false;
}

void Radio1938::RadioExecutionGraph::setEnabled(StageId id, bool value) {
  if (auto* step = findBlock(id)) {
    step->enabled = value;
    return;
  }
  if (auto* step = findSampleControl(id)) {
    step->enabled = value;
    return;
  }
  if (auto* step = findProgramPath(id)) {
    step->enabled = value;
    return;
  }
}

void Radio1938::RadioLifecycle::configure(Radio1938& radio,
                                          RadioInitContext& initCtx) const {
  for (const auto& step : configureSteps) {
    if (!step.init) continue;
    step.init(radio, initCtx);
  }
}

void Radio1938::RadioLifecycle::allocate(Radio1938& radio,
                                         RadioInitContext& initCtx) const {
  for (const auto& step : allocateSteps) {
    if (!step.init) continue;
    step.init(radio, initCtx);
  }
}

void Radio1938::RadioLifecycle::initializeDependentState(
    Radio1938& radio,
    RadioInitContext& initCtx) const {
  for (const auto& step : initializeDependentStateSteps) {
    if (!step.init) continue;
    step.init(radio, initCtx);
  }
}

void Radio1938::RadioLifecycle::resetRuntime(Radio1938& radio) const {
  for (const auto& step : resetSteps) {
    if (!step.reset) continue;
    step.reset(radio);
  }
}

static void updateCalibrationSnapshot(Radio1938& radio);

template <size_t StepCount>
static RadioBlockControl runBlockPrepare(
    Radio1938& radio,
    const std::array<BlockStep, StepCount>& steps,
    uint32_t frames) {
  RadioBlockControl block{};
  for (const auto& step : steps) {
    if (!step.enabled || !step.prepare) continue;
    step.prepare(radio, block, frames);
  }
  return block;
}

template <size_t StepCount>
static void runSampleControl(
    Radio1938& radio,
    RadioSampleContext& ctx,
    const std::array<SampleControlStep, StepCount>& steps) {
  for (const auto& step : steps) {
    if (!step.enabled || !step.update) continue;
    step.update(radio, ctx);
  }
}

template <size_t StepCount>
static float runProgramPath(
    Radio1938& radio,
    float y,
    const RadioSampleContext& ctx,
    const std::array<ProgramPathStep, StepCount>& steps) {
  for (const auto& step : steps) {
    if (!step.enabled || !step.process) continue;
    float in = y;
    y = step.process(radio, y, ctx);
    updateStageCalibration(radio, step.id, in, y);
  }
  return y;
}

template <size_t StepCount>
static float runProgramPathFromIndex(
    Radio1938& radio,
    float y,
    const RadioSampleContext& ctx,
    const std::array<ProgramPathStep, StepCount>& steps,
    size_t startIndex) {
  for (size_t i = startIndex; i < StepCount; ++i) {
    const auto& step = steps[i];
    if (!step.enabled || !step.process) continue;
    float in = y;
    y = step.process(radio, y, ctx);
    updateStageCalibration(radio, step.id, in, y);
  }
  return y;
}

template <size_t StepCount>
static float runProgramPathRange(
    Radio1938& radio,
    float y,
    const RadioSampleContext& ctx,
    const std::array<ProgramPathStep, StepCount>& steps,
    size_t startIndex,
    size_t endIndex) {
  size_t end = std::min(endIndex, static_cast<size_t>(StepCount));
  for (size_t i = startIndex; i < end; ++i) {
    const auto& step = steps[i];
    if (!step.enabled || !step.process) continue;
    float in = y;
    y = step.process(radio, y, ctx);
    updateStageCalibration(radio, step.id, in, y);
  }
  return y;
}

static constexpr size_t kInputProgramStartIndex = 0;
static constexpr size_t kMixerProgramStartIndex = 2;
// FinalLimiter and OutputClip operate in the digital domain (post-scaling).
static constexpr size_t kDigitalProgramStartIndex = 11;

template <typename InputSampleFn, typename OutputSampleFn>
static void processRadioFrames(Radio1938& radio,
                               uint32_t frames,
                               size_t programStartIndex,
                               InputSampleFn&& inputSample,
                               OutputSampleFn&& outputSample) {
  if (frames == 0 || radio.graph.bypass) return;

  radio.diagnostics.reset();
  RadioBlockControl block = runBlockPrepare(radio, radio.graph.blockSteps, frames);
  for (uint32_t frame = 0; frame < frames; ++frame) {
    RadioSampleContext ctx{};
    ctx.block = &block;
    runSampleControl(radio, ctx, radio.graph.sampleControlSteps);
    float x = inputSample(frame);
    // Physical-domain stages (Input through Cabinet).
    size_t physEnd = std::min(kDigitalProgramStartIndex,
                              radio.graph.programPathSteps.size());
    float yPhysical = runProgramPathRange(
        radio, x, ctx, radio.graph.programPathSteps,
        programStartIndex, physEnd);
    // Convert physical speaker volts to digital [-1,1] range.
    float yDigital = scalePhysicalSpeakerVoltsToDigital(radio, yPhysical);
    yDigital = applyDigitalMakeupGain(radio, yDigital);
    // Digital-domain stages (FinalLimiter, OutputClip).
    yDigital = runProgramPathRange(
        radio, yDigital, ctx, radio.graph.programPathSteps,
        physEnd, radio.graph.programPathSteps.size());
    if (radio.calibration.enabled) {
      radio.calibration.maxDigitalOutput =
          std::max(radio.calibration.maxDigitalOutput, std::fabs(yDigital));
    }
    outputSample(frame, yDigital);
  }

  if (radio.calibration.enabled) {
    updateCalibrationSnapshot(radio);
  }
}

static float sampleInterleavedToMono(const float* samples,
                                     uint32_t frame,
                                     int channels) {
  if (!samples) return 0.0f;
  if (channels <= 1) return samples[frame];
  float sum = 0.0f;
  size_t base = static_cast<size_t>(frame) * static_cast<size_t>(channels);
  for (int channel = 0; channel < channels; ++channel) {
    sum += samples[base + static_cast<size_t>(channel)];
  }
  return sum / static_cast<float>(channels);
}

static void writeMonoToInterleaved(float* samples,
                                   uint32_t frame,
                                   int channels,
                                   float y) {
  if (!samples) return;
  if (channels <= 1) {
    samples[frame] = y;
    return;
  }
  size_t base = static_cast<size_t>(frame) * static_cast<size_t>(channels);
  for (int channel = 0; channel < channels; ++channel) {
    samples[base + static_cast<size_t>(channel)] = y;
  }
}

static void applyPhilco37116Preset(Radio1938& radio) {
  radio.identity.driftDepth = 0.06f;
  radio.globals.ifNoiseMix = 0.22f;
  radio.globals.inputPad = 1.0f;
  radio.globals.enableAutoLevel = false;
  radio.globals.autoTargetDb = -21.0f;
  radio.globals.autoMaxBoostDb = 2.5f;

  radio.tuning.safeBwMinHz = 2400.0f;
  radio.tuning.safeBwMaxHz = 9000.0f;
  radio.tuning.preBwScale = 1.00f;
  radio.tuning.postBwScale = 1.00f;
  radio.tuning.smoothTau = 0.05f;
  radio.tuning.updateEps = 0.25f;
  radio.tuning.magneticTuningEnabled = true;
  radio.tuning.afcCaptureHz = 420.0f;
  radio.tuning.afcMaxCorrectionHz = 110.0f;
  radio.tuning.afcDeadband = 0.015f;
  radio.tuning.afcResponseMs = 240.0f;

  radio.frontEnd.inputHpHz = 115.0f;
  radio.frontEnd.rfGain = 1.0f;
  radio.frontEnd.avcGainDepth = 0.18f;
  radio.frontEnd.selectivityPeakHz = 0.0f;
  radio.frontEnd.selectivityPeakQ = 0.707f;
  radio.frontEnd.selectivityPeakGainDb = 0.0f;
  radio.frontEnd.antennaInductanceHenries = 0.011f;
  radio.frontEnd.antennaLoadResistanceOhms = 2200.0f;
  radio.frontEnd.rfInductanceHenries = 0.016f;
  radio.frontEnd.rfLoadResistanceOhms = 3300.0f;

  // Philco 37-116 uses a 6L7G mixer. RCA's 1935 release data gives about
  // 250 V plate supply, 160 V screen, grid-1 bias near -6 V, grid-3 bias near
  // -20 V, 25 V peak oscillator injection, about 3.5 mA plate current, and at
  // least 325 umho conversion conductance. The reduced-order mixer needs a
  // finite DC plate drop for its operating-point fit, so keep the quiescent
  // plate near the same 10 k load order used for the AC conversion model
  // instead of the impossible 250 V-at-3.5 mA no-drop placeholder.
  radio.mixer.rfGridDriveVolts = 1.0f;
  radio.mixer.loGridDriveVolts = 18.0f;
  radio.mixer.loGridBiasVolts = -15.0f;
  radio.mixer.avcGridDriveVolts = 24.0f;
  radio.mixer.plateSupplyVolts = 250.0f;
  radio.mixer.plateDcVolts = 215.0f;
  radio.mixer.screenVolts = 160.0f;
  radio.mixer.biasVolts = -6.0f;
  radio.mixer.cutoffVolts = -45.0f;
  radio.mixer.plateCurrentAmps = 0.0035f;
  radio.mixer.mutualConductanceSiemens = 0.0011f;
  radio.mixer.acLoadResistanceOhms = 10000.0f;
  radio.mixer.plateKneeVolts = 22.0f;
  radio.mixer.gridSoftnessVolts = 2.0f;

  radio.ifStrip.enabled = true;
  radio.ifStrip.ifMinBwHz = 2400.0f;
  radio.ifStrip.primaryInductanceHenries = 0.00022f;
  radio.ifStrip.secondaryInductanceHenries = 0.00025f;
  radio.ifStrip.secondaryLoadResistanceOhms = 680.0f;
  // Reduced-order IF gain stands in for the missing plate-voltage swing of the
  // 470 kHz strip. Calibrate it to land the detector audio in the few-hundred
  // millivolt range before the 6J5/6F6/6B4 chain, rather than recovering that
  // level digitally after the speaker model.
  radio.ifStrip.stageGain = 2.0f;
  radio.ifStrip.avcGainDepth = 0.18f;
  radio.ifStrip.ifCenterHz = 470000.0f;
  radio.ifStrip.interstageCouplingCoeff = 0.15f;
  radio.ifStrip.outputCouplingCoeff = 0.11f;

  radio.demod.am.audioDiodeDrop = 0.0100f;
  radio.demod.am.avcDiodeDrop = 0.0080f;
  radio.demod.am.audioJunctionSlopeVolts = 0.0045f;
  radio.demod.am.avcJunctionSlopeVolts = 0.0040f;
  // Reduced-order 6H6 detector network: detector storage and delayed AVC live
  // here, while the explicit volume/loudness/grid-coupling load is solved in
  // the first-audio receiver stage and applied directly back onto the detector.
  radio.demod.am.detectorStorageCapFarads = 350e-12f;
  radio.demod.am.audioChargeResistanceOhms = 5100.0f;
  radio.demod.am.audioDischargeResistanceOhms = 160000.0f;
  radio.demod.am.avcChargeResistanceOhms = 1000000.0f;
  radio.demod.am.avcDischargeResistanceOhms =
      parallelResistance(1000000.0f, 1000000.0f);
  radio.demod.am.avcFilterCapFarads = 0.15e-6f;
  // Let delayed AVC ride on a multi-volt detector reference so the detector
  // can build a realistic carrier/audio voltage before the RF/IF gain is
  // pinched back.
  radio.demod.am.controlVoltageRef = 3.0f;
  radio.demod.am.senseLowHz = 0.0f;
  radio.demod.am.senseHighHz = 0.0f;
  radio.demod.am.afcSenseLpHz = 34.0f;

  radio.receiverCircuit.enabled = true;
  // Philco 37-116 control 33-5158 is 2 Mohm total with a 1 Mohm tap. The
  // surrounding RC here stays reduced-order, but the control geometry and
  // first-audio tube are now anchored to the 37-116 chassis rather than 116X.
  radio.receiverCircuit.volumeControlResistanceOhms = 2000000.0f;
  radio.receiverCircuit.volumeControlTapResistanceOhms = 1000000.0f;
  radio.receiverCircuit.volumeControlPosition = 1.0f;
  radio.receiverCircuit.volumeControlLoudnessResistanceOhms = 490000.0f;
  radio.receiverCircuit.volumeControlLoudnessCapFarads = 0.015e-6f;
  radio.receiverCircuit.couplingCapFarads = 0.01e-6f;
  radio.receiverCircuit.gridLeakResistanceOhms = 1000000.0f;
  // The 37-116 first audio stage is a 6J5G triode. A common 250 V / -8 V
  // operating point with about 9 mA plate current, rp about 7.7k, gm about
  // 2600 umho, and mu about 20 puts the plate near the roughly 90 V socket
  // reading shown in the service data with a ~15k plate load.
  radio.receiverCircuit.tubePlateSupplyVolts = 250.0f;
  radio.receiverCircuit.tubePlateDcVolts = 90.0f;
  radio.receiverCircuit.tubeScreenVolts = 0.0f;
  radio.receiverCircuit.tubeBiasVolts = -8.0f;
  radio.receiverCircuit.tubePlateCurrentAmps = 0.009f;
  radio.receiverCircuit.tubeMutualConductanceSiemens = 0.0026f;
  radio.receiverCircuit.tubeMu = 20.0f;
  radio.receiverCircuit.tubeTriodeConnected = true;
  radio.receiverCircuit.tubeLoadResistanceOhms = 15000.0f;
  radio.receiverCircuit.tubePlateKneeVolts = 16.0f;
  radio.receiverCircuit.tubeGridSoftnessVolts = 0.6f;

  radio.tone.presenceHz = 0.0f;
  radio.tone.presenceQ = 0.78f;
  radio.tone.presenceGainDb = 0.0f;
  radio.tone.tiltSplitHz = 0.0f;

  radio.power.sagStart = 0.06f;
  radio.power.sagEnd = 0.22f;
  radio.power.rippleDepth = 0.01f;
  radio.power.sagAttackMs = 60.0f;
  radio.power.sagReleaseMs = 900.0f;
  radio.power.rectifierMinHz = 80.0f;
  radio.power.rippleSecondHarmonicMix = 0.0f;
  radio.power.gainSagPerPower = 0.015f;
  radio.power.rippleGainBase = 0.20f;
  radio.power.rippleGainDepth = 0.30f;
  radio.power.gainMin = 0.92f;
  radio.power.gainMax = 1.02f;
  radio.power.supplyDriveDepth = 0.01f;
  radio.power.supplyBiasDepth = 0.0f;
  radio.power.postLpHz = 0.0f;
  // The 37-116 driver is a triode-connected 6F6G feeding the 6B4G pair through
  // an interstage transformer. Tung-Sol RC-13 gives 250 V plate, 250 V screen,
  // -20 V grid, about 21 mA plate current, mu about 6.8, gm about 2600 umho,
  // and 4000 ohm load for single-ended class-A triode operation.
  radio.power.gridCouplingCapFarads = 0.05e-6f;
  radio.power.gridLeakResistanceOhms = 100000.0f;
  radio.power.tubePlateSupplyVolts = 250.0f;
  radio.power.tubeScreenVolts = 250.0f;
  radio.power.tubeBiasVolts = -20.0f;
  radio.power.tubePlateCurrentAmps = 0.021f;
  radio.power.tubeMutualConductanceSiemens = 0.0026f;
  radio.power.tubeMu = 6.8f;
  radio.power.tubeTriodeConnected = true;
  radio.power.tubeAcLoadResistanceOhms = 4000.0f;
  radio.power.tubePlateKneeVolts = 24.0f;
  radio.power.tubeGridSoftnessVolts = 0.8f;
  radio.power.tubeGridCurrentResistanceOhms = 1000.0f;
  radio.power.outputGridLeakResistanceOhms = 250000.0f;
  radio.power.outputGridCurrentResistanceOhms = 1200.0f;
  radio.power.interstagePrimaryLeakageInductanceHenries = 0.45f;
  radio.power.interstageMagnetizingInductanceHenries = 15.0f;
  radio.power.interstagePrimaryResistanceOhms = 430.0f;
  radio.power.tubePlateDcVolts =
      radio.power.tubePlateSupplyVolts -
      radio.power.tubePlateCurrentAmps *
          radio.power.interstagePrimaryResistanceOhms;
  // The 6F6 driver must voltage-step up into the fixed-bias 6B4G pair; the
  // earlier 1:1.4 and 1:3 equivalents still left each 6B4 grid far below the
  // AB1 drive window. In this center-tapped model the ratio is primary to full
  // secondary, so a 1:6 transformer means about 1:3 step-up to each grid half.
  // Keep enough voltage gain for the 6B4 pair, but avoid using the interstage
  // transformer itself as a hidden loudness boost.
  radio.power.interstageTurnsRatioPrimaryToSecondary = 1.0f / 6.0f;
  radio.power.interstagePrimaryCoreLossResistanceOhms = 220000.0f;
  // The pF stray capacitances are ultrasonic details in the real chassis.
  // In this 48 kHz reduced-order audio model they destabilize the transformer
  // solve and collapse the audible output, so the preset leaves them out.
  radio.power.interstagePrimaryShuntCapFarads = 0.0f;
  radio.power.interstageSecondaryLeakageInductanceHenries = 0.040f;
  radio.power.interstageSecondaryResistanceOhms = 296.0f;
  radio.power.interstageSecondaryShuntCapFarads = 0.0f;
  radio.power.interstageIntegrationSubsteps = 8;
  // The 37-116 uses push-pull 6B4G output tubes. Tung-Sol gives the fixed-bias
  // AB1 pair at 325 V plate, -68 V grid, 40 mA zero-signal plate current per
  // tube, and 3000 ohm plate-to-plate load for 15 W output. The 6B4G is the
  // octal 2A3 family member, so keep the same mu/gm class but bias it at the
  // published Philco-era AB1 point instead of the earlier too-hot -39 V guess.
  radio.power.outputTubePlateSupplyVolts = 325.0f;
  radio.power.outputTubeBiasVolts = -68.0f;
  radio.power.outputTubePlateCurrentAmps = 0.040f;
  radio.power.outputTubeMutualConductanceSiemens = 0.00525f;
  radio.power.outputTubeMu = 4.2f;
  radio.power.outputTubePlateToPlateLoadOhms = 3000.0f;
  radio.power.outputTubePlateKneeVolts = 18.0f;
  radio.power.outputTubeGridSoftnessVolts = 2.0f;
  radio.power.outputTransformerPrimaryLeakageInductanceHenries = 35e-3f;
  radio.power.outputTransformerMagnetizingInductanceHenries = 20.0f;
  radio.power.outputTransformerTurnsRatioPrimaryToSecondary =
      std::sqrt(3000.0f / 3.9f);
  radio.power.outputTransformerPrimaryResistanceOhms = 235.0f;
  radio.power.outputTubePlateDcVolts =
      radio.power.outputTubePlateSupplyVolts -
      radio.power.outputTubePlateCurrentAmps *
          (0.5f * radio.power.outputTransformerPrimaryResistanceOhms);
  radio.power.outputTransformerPrimaryCoreLossResistanceOhms = 90000.0f;
  radio.power.outputTransformerPrimaryShuntCapFarads = 0.0f;
  radio.power.outputTransformerSecondaryLeakageInductanceHenries = 60e-6f;
  radio.power.outputTransformerSecondaryResistanceOhms = 0.32f;
  radio.power.outputTransformerSecondaryShuntCapFarads = 0.0f;
  radio.power.outputTransformerIntegrationSubsteps = 8;
  radio.power.outputLoadResistanceOhms = 3.9f;
  // The clean output power reference is derived from the modeled 6B4/output
  // transformer combination during RadioPowerNode::init, not hard-coded here.
  radio.power.nominalOutputPowerWatts = 0.0f;
  assert(std::fabs(radio.power.tubePlateSupplyVolts -
                   radio.power.tubePlateCurrentAmps *
                       radio.power.interstagePrimaryResistanceOhms -
                   radio.power.tubePlateDcVolts) < 1.0f);
  assert(std::fabs(radio.power.outputTubePlateSupplyVolts -
                   radio.power.outputTubePlateCurrentAmps *
                       (0.5f * radio.power.outputTransformerPrimaryResistanceOhms) -
                   radio.power.outputTubePlateDcVolts) < 1.0f);
  radio.output.digitalMakeupGain = 1.0f;

  // --- Global oversampling and output clip settings ---
  // The speaker, limiter and output-clip stages use processOversampled2x, so
  // the oversample factor for biquad anti-alias filters is 2.
  radio.globals.oversampleFactor = 2.0f;
  radio.globals.oversampleCutoffFraction = 0.45f;
  radio.globals.outputClipThreshold = 1.0f;
  radio.globals.postNoiseMix = 0.35f;
  radio.globals.noiseFloorAmp = 0.0f;

  // --- Noise configuration (mains frequency feeds the physical ripple model;
  // hiss/crackle remain explicit stochastic stages) ---
  radio.noiseConfig.enableHumTone = false;
  radio.noiseConfig.humHzDefault = 60.0f;
  radio.noiseConfig.noiseWeightRef = 0.15f;
  radio.noiseConfig.noiseWeightScaleMax = 2.0f;
  radio.noiseConfig.humAmpScale = 0.0f;
  radio.noiseConfig.crackleAmpScale = 0.025f;
  radio.noiseConfig.crackleRateScale = 1.2f;

  // --- Noise shaping (tube hiss spectral character for 1930s receiver) ---
  radio.noiseRuntime.hum.noiseHpHz = 500.0f;
  radio.noiseRuntime.hum.noiseLpHz = 5500.0f;
  radio.noiseRuntime.hum.filterQ = kRadioBiquadQ;
  radio.noiseRuntime.hum.scAttackMs = 2.0f;
  radio.noiseRuntime.hum.scReleaseMs = 80.0f;
  radio.noiseRuntime.hum.crackleDecayMs = 8.0f;
  radio.noiseRuntime.hum.sidechainMaskRef = 0.15f;
  radio.noiseRuntime.hum.hissMaskDepth = 0.5f;
  radio.noiseRuntime.hum.burstMaskDepth = 0.3f;
  radio.noiseRuntime.hum.pinkFastPole = 0.85f;
  radio.noiseRuntime.hum.pinkSlowPole = 0.97f;
  radio.noiseRuntime.hum.brownStep = 0.02f;
  radio.noiseRuntime.hum.hissDriftPole = 0.9992f;
  radio.noiseRuntime.hum.hissDriftNoise = 0.002f;
  radio.noiseRuntime.hum.hissDriftSlowPole = 0.9998f;
  radio.noiseRuntime.hum.hissDriftSlowNoise = 0.001f;
  radio.noiseRuntime.hum.whiteMix = 0.35f;
  radio.noiseRuntime.hum.pinkFastMix = 0.45f;
  radio.noiseRuntime.hum.pinkDifferenceMix = 0.12f;
  radio.noiseRuntime.hum.pinkFastSubtract = 0.6f;
  radio.noiseRuntime.hum.brownMix = 0.08f;
  radio.noiseRuntime.hum.hissBase = 0.7f;
  radio.noiseRuntime.hum.hissDriftDepth = 0.3f;
  radio.noiseRuntime.hum.hissDriftSlowMix = 0.04f;
  radio.noiseRuntime.hum.humSecondHarmonicMix = 0.42f;

  // --- Speaker: 12" electrodynamic field-coil driver (Philco 37-116) ---
  radio.speakerStage.drive = 1.0f;
  radio.speakerStage.speaker.suspensionHz = 65.0f;
  radio.speakerStage.speaker.suspensionQ = 0.90f;
  radio.speakerStage.speaker.suspensionGainDb = 2.2f;
  radio.speakerStage.speaker.coneBodyHz = 1200.0f;
  radio.speakerStage.speaker.coneBodyQ = 0.50f;
  radio.speakerStage.speaker.coneBodyGainDb = 0.25f;
  radio.speakerStage.speaker.topLpHz = 3800.0f;
  radio.speakerStage.speaker.filterQ = kRadioBiquadQ;
  radio.speakerStage.speaker.drive = 1.0f;
  radio.speakerStage.speaker.limit = 0.0f;
  radio.speakerStage.speaker.excursionRef = 8.0f;
  radio.speakerStage.speaker.complianceLossDepth = 0.05f;

  // --- Cabinet: large floor-model console (Philco 37-116) ---
  radio.cabinet.enabled = true;
  radio.cabinet.panelHz = 180.0f;
  radio.cabinet.panelQ = 1.25f;
  radio.cabinet.panelGainDb = 1.0f;
  radio.cabinet.chassisHz = 650.0f;
  radio.cabinet.chassisQ = 0.80f;
  radio.cabinet.chassisGainDb = -0.8f;
  radio.cabinet.cavityDipHz = 900.0f;
  radio.cabinet.cavityDipQ = 1.6f;
  radio.cabinet.cavityDipGainDb = -1.6f;
  radio.cabinet.grilleLpHz = 5000.0f;
  radio.cabinet.rearDelayMs = 0.90f;
  radio.cabinet.rearMix = 0.08f;
  radio.cabinet.rearHpHz = 200.0f;
  radio.cabinet.rearLpHz = 2600.0f;

  // --- Final limiter (digital safety brick-wall) ---
  radio.finalLimiter.enabled = false;
  radio.finalLimiter.threshold = 1.0f;
  radio.finalLimiter.lookaheadMs = 2.0f;
  radio.finalLimiter.attackMs = 0.5f;
  radio.finalLimiter.releaseMs = 80.0f;
}

static void applySetIdentity(Radio1938& radio) {
  auto& identity = radio.identity;
  float drift = std::clamp(identity.driftDepth, 0.0f, 0.25f);
  identity.frontEndAntennaDrift =
      std::clamp(applySeededDrift(1.0f, 0.45f * drift, identity.seed, 0x4101u),
                 0.78f, 1.22f);
  identity.frontEndRfDrift =
      std::clamp(applySeededDrift(1.0f, 0.40f * drift, identity.seed, 0x4102u),
                 0.80f, 1.20f);
  identity.mixerDriveDrift =
      std::clamp(applySeededDrift(1.0f, 0.28f * drift, identity.seed, 0x4103u),
                 0.86f, 1.14f);
  identity.mixerBiasDrift =
      std::clamp(applySeededDrift(1.0f, 0.18f * drift, identity.seed, 0x4104u),
                 0.90f, 1.10f);
  identity.ifPrimaryDrift =
      std::clamp(applySeededDrift(1.0f, 0.35f * drift, identity.seed, 0x4105u),
                 0.84f, 1.16f);
  identity.ifSecondaryDrift =
      std::clamp(applySeededDrift(1.0f, 0.35f * drift, identity.seed, 0x4106u),
                 0.84f, 1.16f);
  identity.ifCouplingDrift =
      std::clamp(applySeededDrift(1.0f, 0.40f * drift, identity.seed, 0x4107u),
                 0.82f, 1.18f);
  identity.detectorLoadDrift =
      std::clamp(applySeededDrift(1.0f, 0.30f * drift, identity.seed, 0x4108u),
                 0.88f, 1.12f);
}

static void refreshIdentityDependentStages(Radio1938& radio) {
  applySetIdentity(radio);
  RadioInitContext initCtx{};
  RadioMixerNode::init(radio, initCtx);
  RadioMixerNode::reset(radio);
  RadioIFStripNode::init(radio, initCtx);
  RadioIFStripNode::reset(radio);
  RadioReceiverCircuitNode::init(radio, initCtx);
  RadioReceiverCircuitNode::reset(radio);
  RadioPowerNode::init(radio, initCtx);
  RadioPowerNode::reset(radio);
  if (radio.calibration.enabled) {
    radio.resetCalibration();
  }
}

std::string_view Radio1938::presetName(Preset preset) {
  switch (preset) {
    case Preset::Philco37116:
      return "philco_37_116";
  }
  return "philco_37_116";
}

Radio1938::Radio1938() { applyPreset(preset); }

std::string_view Radio1938::stageName(StageId id) {
  switch (id) {
    case StageId::Tuning:
      return "MagneticTuning";
    case StageId::Input:
      return "Input";
    case StageId::AVC:
      return "AVC";
    case StageId::AFC:
      return "AFC";
    case StageId::ControlBus:
      return "ControlBus";
    case StageId::InterferenceDerived:
      return "InterferenceDerived";
    case StageId::FrontEnd:
      return "RFFrontEnd";
    case StageId::Mixer:
      return "Mixer";
    case StageId::IFStrip:
      return "IFStrip";
    case StageId::Demod:
      return "Detector";
    case StageId::ReceiverCircuit:
      return "AudioStage";
    case StageId::Tone:
      return "Tone";
    case StageId::Power:
      return "Power";
    case StageId::Noise:
      return "Noise";
    case StageId::Speaker:
      return "Speaker";
    case StageId::Cabinet:
      return "Cabinet";
    case StageId::FinalLimiter:
      return "FinalLimiter";
    case StageId::OutputClip:
      return "OutputClip";
  }
  return "Unknown";
}

bool Radio1938::applyPreset(std::string_view presetNameValue) {
  if (presetNameValue == "philco_37_116") {
    applyPreset(Preset::Philco37116);
    return true;
  }
  if (presetNameValue == "philco_37_116x") {
    applyPreset(Preset::Philco37116);
    return true;
  }
  return false;
}

void Radio1938::applyPreset(Preset presetValue) {
  preset = presetValue;
  switch (presetValue) {
    case Preset::Philco37116:
      applyPhilco37116Preset(*this);
      break;
  }
  if (!initialized) return;
  init(channels, sampleRate, bwHz, noiseWeight);
}

void Radio1938::setIdentitySeed(uint32_t seed) {
  identity.seed = seed;
  if (!initialized) return;
  refreshIdentityDependentStages(*this);
}

void Radio1938::setCalibrationEnabled(bool enabled) {
  calibration.enabled = enabled;
  resetCalibration();
}

void Radio1938::resetCalibration() {
  calibration.reset();
}

void Radio1938::init(int ch, float sr, float bw, float noise) {
  channels = ch;
  sampleRate = sr;
  bwHz = bw;
  noiseWeight = noise;
  applySetIdentity(*this);
  RadioInitContext initCtx{};
  lifecycle.configure(*this, initCtx);
  lifecycle.allocate(*this, initCtx);
  lifecycle.initializeDependentState(*this, initCtx);
  initialized = true;
  if (calibration.enabled) {
    resetCalibration();
  }
  reset();
}

void Radio1938::reset() {
  diagnostics.reset();
  iqInput.resetRuntime();
  sourceFrame.resetRuntime();
  lifecycle.resetRuntime(*this);
  if (calibration.enabled) {
    resetCalibration();
  }
}

void Radio1938::processIfReal(float* samples, uint32_t frames) {
  if (!samples || frames == 0) return;
  processRadioFrames(
      *this, frames, kInputProgramStartIndex,
      [&](uint32_t frame) {
        float x = sampleInterleavedToMono(samples, frame, channels);
        sourceFrame.setRealRf(x);
        return x;
      },
      [&](uint32_t frame, float y) {
        writeMonoToInterleaved(samples, frame, channels, y);
      });
}

void Radio1938::processAmAudio(const float* audioSamples,
                               float* outSamples,
                               uint32_t frames,
                               float receivedCarrierRmsVolts,
                               float modulationIndex) {
  if (!audioSamples || !outSamples || frames == 0) return;
  std::vector<float> rfScratch(frames);
  float safeSampleRate = std::max(sampleRate, 1.0f);
  float carrierHz =
      std::clamp(ifStrip.sourceCarrierHz, 1000.0f, safeSampleRate * 0.45f);
  float carrierStep = kRadioTwoPi * (carrierHz / safeSampleRate);
  float carrierPeak =
      std::sqrt(2.0f) * std::max(receivedCarrierRmsVolts, 0.0f);
  float phase = iqInput.iqPhase;
  for (uint32_t frame = 0; frame < frames; ++frame) {
    float envelopeFactor =
        std::max(0.0f, 1.0f + modulationIndex * audioSamples[frame]);
    float envelope = carrierPeak * envelopeFactor;
    rfScratch[frame] = envelope * std::cos(phase);
    phase += carrierStep;
    if (phase >= kRadioTwoPi) phase -= kRadioTwoPi;
  }
  iqInput.iqPhase = phase;
  processIfReal(rfScratch.data(), frames);
  std::copy(rfScratch.begin(), rfScratch.end(), outSamples);
}

void Radio1938::processIqBaseband(const float* iqInterleaved,
                                  float* outSamples,
                                  uint32_t frames) {
  if (!iqInterleaved || !outSamples || frames == 0) return;
  processRadioFrames(
      *this, frames, kMixerProgramStartIndex,
      [&](uint32_t frame) {
        size_t base = static_cast<size_t>(frame) * 2u;
        float i = iqInterleaved[base];
        float q = iqInterleaved[base + 1u];
        sourceFrame.setComplexEnvelope(i, q);
        return i;
      },
      [&](uint32_t frame, float y) {
        writeMonoToInterleaved(outSamples, frame, channels, y);
      });
}
