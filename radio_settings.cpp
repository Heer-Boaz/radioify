#include "radio.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <map>
#include <limits>

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

static std::string lowerIniName(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

struct IniAudioFilterSetting {
  int line = 0;
  std::string key;
  std::string value;
};
using IniAudioFilterPreset = std::map<std::string, std::vector<IniAudioFilterSetting>>;
using IniAudioFilterConfig = std::map<std::string, IniAudioFilterPreset>;

static bool endsWithIgnoreCase(std::string value, std::string suffix) {
  if (suffix.empty()) return false;
  if (value.size() < suffix.size()) return false;
  for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (char& c : suffix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value.substr(value.size() - suffix.size()) == suffix;
}

static bool isKnownRadioSection(const std::string& section) {
  return section == "globals" ||
         section == "tuning" || section == "frontend" ||
         section == "mixer" || section == "ifstrip" || section == "demod" ||
         section == "receivercircuit" || section == "tone" || section == "power" ||
         section == "noise" || section == "speaker" ||
         section == "cabinet" || section == "finallimiter" ||
         section == "output" || section == "nodes";
}

static bool applyRadioSettingsValue(Radio1938& radio,
                                   const std::string& sectionNorm,
                                   const std::string& keyNorm,
                                   const std::string& valueText,
                                   int lineNumber,
                                   std::string* error);

static bool parseIniBool(const std::string& valueText,
                        bool& value,
                        std::string* error,
                        int lineNumber,
                        const std::string& settingName);

static std::string iniError(int line, const std::string& details);

static bool applyAudioFilterNodeSetting(Radio1938& radio,
                                       const std::string& keyNorm,
                                       const std::string& valueText,
                                       int lineNumber,
                                       std::string* error) {
  auto setNode = [&](StageId id, const std::string& settingName) {
    bool parsed = false;
    if (!parseIniBool(valueText, parsed, error, lineNumber, settingName)) {
      return false;
    }
    radio.graph.setEnabled(id, parsed);
    return true;
  };

  if (keyNorm == "tuning") return setNode(StageId::Tuning, "nodes.tuning");
  if (keyNorm == "input") return setNode(StageId::Input, "nodes.input");
  if (keyNorm == "avc") return setNode(StageId::AVC, "nodes.avc");
  if (keyNorm == "afc") return setNode(StageId::AFC, "nodes.afc");
  if (keyNorm == "controlbus") {
    return setNode(StageId::ControlBus, "nodes.controlbus");
  }
  if (keyNorm == "interferencederived") {
    return setNode(StageId::InterferenceDerived, "nodes.interferencederived");
  }
  if (keyNorm == "frontend") return setNode(StageId::FrontEnd, "nodes.frontend");
  if (keyNorm == "mixer") return setNode(StageId::Mixer, "nodes.mixer");
  if (keyNorm == "ifstrip") return setNode(StageId::IFStrip, "nodes.ifstrip");
  if (keyNorm == "demod") return setNode(StageId::Demod, "nodes.demod");
  if (keyNorm == "receivercircuit") {
    return setNode(StageId::ReceiverCircuit, "nodes.receivercircuit");
  }
  if (keyNorm == "tone") return setNode(StageId::Tone, "nodes.tone");
  if (keyNorm == "power") return setNode(StageId::Power, "nodes.power");
  if (keyNorm == "noise") return setNode(StageId::Noise, "nodes.noise");
  if (keyNorm == "speaker") return setNode(StageId::Speaker, "nodes.speaker");
  if (keyNorm == "cabinet") return setNode(StageId::Cabinet, "nodes.cabinet");
  if (keyNorm == "finallimiter") {
    return setNode(StageId::FinalLimiter, "nodes.finallimiter");
  }
  if (keyNorm == "outputclip") {
    return setNode(StageId::OutputClip, "nodes.outputclip");
  }

  if (error) {
    *error = iniError(lineNumber, "unknown node '" + keyNorm + "' in settings file");
  }
  return false;
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
  if (section == "globals") {
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
    if (keyNorm == "satclipdelta") {
      return setFloat(radio.globals.satClipDelta, "globals.satClipDelta");
    }
    if (keyNorm == "satclipminlevel") {
      return setFloat(radio.globals.satClipMinLevel,
                      "globals.satClipMinLevel");
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
    if (keyNorm == "primarycapacitancefarads") {
      return setFloat(radio.ifStrip.primaryCapacitanceFarads,
                      "ifStrip.primaryCapacitanceFarads");
    }
    if (keyNorm == "primaryresistanceohms") {
      return setFloat(radio.ifStrip.primaryResistanceOhms,
                      "ifStrip.primaryResistanceOhms");
    }
    if (keyNorm == "secondaryinductancehenries") {
      return setFloat(radio.ifStrip.secondaryInductanceHenries,
                      "ifStrip.secondaryInductanceHenries");
    }
    if (keyNorm == "secondarycapacitancefarads") {
      return setFloat(radio.ifStrip.secondaryCapacitanceFarads,
                      "ifStrip.secondaryCapacitanceFarads");
    }
    if (keyNorm == "secondaryresistanceohms") {
      return setFloat(radio.ifStrip.secondaryResistanceOhms,
                      "ifStrip.secondaryResistanceOhms");
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
    if (keyNorm == "oversamplefactor") {
      return setInt(radio.ifStrip.oversampleFactor,
                      "ifStrip.oversampleFactor");
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
    if (keyNorm == "detectorplatecouplingcapfarads") {
      return setFloat(radio.demod.am.detectorPlateCouplingCapFarads,
                      "demod.detectorPlateCouplingCapFarads");
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
    if (keyNorm == "satosprev") {
      return setFloat(radio.power.satOsPrev, "power.satOsPrev");
    }
    if (keyNorm == "satdrive") return setFloat(radio.power.satDrive, "power.satDrive");
    if (keyNorm == "satmix") return setFloat(radio.power.satMix, "power.satMix");
    if (keyNorm == "sagattackms") {
      return setFloat(radio.power.sagAttackMs, "power.sagAttackMs");
    }
    if (keyNorm == "sagreleasems") {
      return setFloat(radio.power.sagReleaseMs, "power.sagReleaseMs");
    }
    if (keyNorm == "rectifierminhz") {
      return setFloat(radio.power.rectifierMinHz, "power.rectifierMinHz");
    }
    if (keyNorm == "ripplesecondharmonicmix" ||
        keyNorm == "rippelesecondharmonicmix") {
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
    if (keyNorm == "interstageturnsratioprimarytosecondary") {
      return setFloat(radio.power.interstageTurnsRatioPrimaryToSecondary,
                      "power.interstageTurnsRatioPrimaryToSecondary");
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
    if (keyNorm == "noisehphz") {
      return setFloat(radio.noiseRuntime.hum.noiseHpHz, "noise.noiseHpHz");
    }
    if (keyNorm == "noiselphz") {
      return setFloat(radio.noiseRuntime.hum.noiseLpHz, "noise.noiseLpHz");
    }
    if (keyNorm == "filterq") {
      return setFloat(radio.noiseRuntime.hum.filterQ, "noise.filterQ");
    }
    if (keyNorm == "scattackms") {
      return setFloat(radio.noiseRuntime.hum.scAttackMs, "noise.scAttackMs");
    }
    if (keyNorm == "screleasems") {
      return setFloat(radio.noiseRuntime.hum.scReleaseMs, "noise.scReleaseMs");
    }
    if (keyNorm == "crackledecayms") {
      return setFloat(radio.noiseRuntime.hum.crackleDecayMs,
                      "noise.crackleDecayMs");
    }
    if (keyNorm == "sidechainmaskref") {
      return setFloat(radio.noiseRuntime.hum.sidechainMaskRef,
                      "noise.sidechainMaskRef");
    }
    if (keyNorm == "hissmaskdepth") {
      return setFloat(radio.noiseRuntime.hum.hissMaskDepth,
                      "noise.hissMaskDepth");
    }
    if (keyNorm == "burstmaskdepth") {
      return setFloat(radio.noiseRuntime.hum.burstMaskDepth,
                      "noise.burstMaskDepth");
    }
    if (keyNorm == "pinkfastpole") {
      return setFloat(radio.noiseRuntime.hum.pinkFastPole,
                      "noise.pinkFastPole");
    }
    if (keyNorm == "pinkslowpole") {
      return setFloat(radio.noiseRuntime.hum.pinkSlowPole,
                      "noise.pinkSlowPole");
    }
    if (keyNorm == "brownstep") {
      return setFloat(radio.noiseRuntime.hum.brownStep, "noise.brownStep");
    }
    if (keyNorm == "hissdriftpole") {
      return setFloat(radio.noiseRuntime.hum.hissDriftPole,
                      "noise.hissDriftPole");
    }
    if (keyNorm == "hissdriftnoise") {
      return setFloat(radio.noiseRuntime.hum.hissDriftNoise,
                      "noise.hissDriftNoise");
    }
    if (keyNorm == "hissdriftslowpole") {
      return setFloat(radio.noiseRuntime.hum.hissDriftSlowPole,
                      "noise.hissDriftSlowPole");
    }
    if (keyNorm == "hissdriftslownoise") {
      return setFloat(radio.noiseRuntime.hum.hissDriftSlowNoise,
                      "noise.hissDriftSlowNoise");
    }
    if (keyNorm == "whitemix") {
      return setFloat(radio.noiseRuntime.hum.whiteMix, "noise.whiteMix");
    }
    if (keyNorm == "pinkfastmix") {
      return setFloat(radio.noiseRuntime.hum.pinkFastMix,
                      "noise.pinkFastMix");
    }
    if (keyNorm == "pinkdifferencemix") {
      return setFloat(radio.noiseRuntime.hum.pinkDifferenceMix,
                      "noise.pinkDifferenceMix");
    }
    if (keyNorm == "pinkfastsubtract") {
      return setFloat(radio.noiseRuntime.hum.pinkFastSubtract,
                      "noise.pinkFastSubtract");
    }
    if (keyNorm == "brownmix") {
      return setFloat(radio.noiseRuntime.hum.brownMix, "noise.brownMix");
    }
    if (keyNorm == "hissbase") {
      return setFloat(radio.noiseRuntime.hum.hissBase, "noise.hissBase");
    }
    if (keyNorm == "hissdriftdepth") {
      return setFloat(radio.noiseRuntime.hum.hissDriftDepth,
                      "noise.hissDriftDepth");
    }
    if (keyNorm == "hissdriftslowmix") {
      return setFloat(radio.noiseRuntime.hum.hissDriftSlowMix,
                      "noise.hissDriftSlowMix");
    }
    if (keyNorm == "humsecondharmonicmix") {
      return setFloat(radio.noiseRuntime.hum.humSecondHarmonicMix,
                      "noise.humSecondHarmonicMix");
    }
    return false;
  }

  if (section == "speaker") {
    if (keyNorm == "drive") {
      return setFloat(radio.speakerStage.drive, "speaker.drive");
    }
    if (keyNorm == "suspensionhz") {
      return setFloat(radio.speakerStage.speaker.suspensionHz,
                      "speaker.suspensionHz");
    }
    if (keyNorm == "suspensionq") {
      return setFloat(radio.speakerStage.speaker.suspensionQ,
                      "speaker.suspensionQ");
    }
    if (keyNorm == "suspensiongaindb") {
      return setFloat(radio.speakerStage.speaker.suspensionGainDb,
                      "speaker.suspensionGainDb");
    }
    if (keyNorm == "conebodyhz") {
      return setFloat(radio.speakerStage.speaker.coneBodyHz,
                      "speaker.coneBodyHz");
    }
    if (keyNorm == "conebodyq") {
      return setFloat(radio.speakerStage.speaker.coneBodyQ,
                      "speaker.coneBodyQ");
    }
    if (keyNorm == "conebodygaindb") {
      return setFloat(radio.speakerStage.speaker.coneBodyGainDb,
                      "speaker.coneBodyGainDb");
    }
    if (keyNorm == "toplphz") {
      return setFloat(radio.speakerStage.speaker.topLpHz, "speaker.topLpHz");
    }
    if (keyNorm == "filterq") {
      return setFloat(radio.speakerStage.speaker.filterQ, "speaker.filterQ");
    }
    if (keyNorm == "limit") {
      return setFloat(radio.speakerStage.speaker.limit, "speaker.limit");
    }
    if (keyNorm == "excursionref") {
      return setFloat(radio.speakerStage.speaker.excursionRef,
                      "speaker.excursionRef");
    }
    if (keyNorm == "compliancelossdepth") {
      return setFloat(radio.speakerStage.speaker.complianceLossDepth,
                      "speaker.complianceLossDepth");
    }
    return false;
  }

  if (section == "cabinet") {
    if (keyNorm == "enabled") {
      return setBool(radio.cabinet.enabled, "cabinet.enabled");
    }
    if (keyNorm == "panelhz") {
      return setFloat(radio.cabinet.panelHz, "cabinet.panelHz");
    }
    if (keyNorm == "panelq") {
      return setFloat(radio.cabinet.panelQ, "cabinet.panelQ");
    }
    if (keyNorm == "panelgaindb") {
      return setFloat(radio.cabinet.panelGainDb, "cabinet.panelGainDb");
    }
    if (keyNorm == "chassishz") {
      return setFloat(radio.cabinet.chassisHz, "cabinet.chassisHz");
    }
    if (keyNorm == "chassisq") {
      return setFloat(radio.cabinet.chassisQ, "cabinet.chassisQ");
    }
    if (keyNorm == "chassisgaindb") {
      return setFloat(radio.cabinet.chassisGainDb, "cabinet.chassisGainDb");
    }
    if (keyNorm == "cavitydiphz") {
      return setFloat(radio.cabinet.cavityDipHz, "cabinet.cavityDipHz");
    }
    if (keyNorm == "cavitydipq") {
      return setFloat(radio.cabinet.cavityDipQ, "cabinet.cavityDipQ");
    }
    if (keyNorm == "cavitydipgaindb") {
      return setFloat(radio.cabinet.cavityDipGainDb,
                      "cabinet.cavityDipGainDb");
    }
    if (keyNorm == "grillelphz") {
      return setFloat(radio.cabinet.grilleLpHz, "cabinet.grilleLpHz");
    }
    if (keyNorm == "reardelayms") {
      return setFloat(radio.cabinet.rearDelayMs, "cabinet.rearDelayMs");
    }
    if (keyNorm == "rearmix") {
      return setFloat(radio.cabinet.rearMix, "cabinet.rearMix");
    }
    if (keyNorm == "rearhphz") {
      return setFloat(radio.cabinet.rearHpHz, "cabinet.rearHpHz");
    }
    if (keyNorm == "rearlphz") {
      return setFloat(radio.cabinet.rearLpHz, "cabinet.rearLpHz");
    }
    return false;
  }

  if (section == "finallimiter") {
    if (keyNorm == "enabled") {
      return setBool(radio.finalLimiter.enabled, "finalLimiter.enabled");
    }
    if (keyNorm == "threshold") {
      return setFloat(radio.finalLimiter.threshold, "finalLimiter.threshold");
    }
    if (keyNorm == "lookaheadms") {
      return setFloat(radio.finalLimiter.lookaheadMs,
                      "finalLimiter.lookaheadMs");
    }
    if (keyNorm == "attackms") {
      return setFloat(radio.finalLimiter.attackMs, "finalLimiter.attackMs");
    }
    if (keyNorm == "releasems") {
      return setFloat(radio.finalLimiter.releaseMs, "finalLimiter.releaseMs");
    }
    return false;
  }

  if (section == "output") {
    if (keyNorm == "digitalmakeupgain") {
      return setFloat(radio.output.digitalMakeupGain,
                      "output.digitalMakeupGain");
    }
    return false;
  }

  if (section == "nodes") {
    return applyAudioFilterNodeSetting(radio, keyNorm, valueText, lineNumber, error);
  }

  if (error) {
    *error = iniError(lineNumber, "unknown setting '" + section + "." + keyNorm +
                                         "' in settings file");
  }
  return false;
}

static bool parseAudioFilterIniSettings(const std::string& path,
                                       IniAudioFilterConfig& presets,
                                       std::string* error) {
  std::ifstream settingsFile(path);
  if (!settingsFile.is_open()) {
    if (error) {
      *error = "Failed to open radio settings file: " + path;
    }
    return false;
  }

  int lineNumber = 0;
  std::string presetName;
  std::string section;
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
      std::string sectionNameLower = lowerIniName(sectionName);
      const std::string prefix = "audio-filter.";
      if (sectionNameLower.rfind(prefix, 0) != 0) {
        if (error) {
          *error = iniError(
              lineNumber,
              "unknown section '" + sectionName +
                  "', expected [audio-filter.<preset>.<section>]");
        }
        return false;
      }

      std::string tail = sectionName.substr(prefix.size());
      size_t separator = tail.find('.');
      if (separator == std::string::npos || separator == 0 ||
          separator + 1 >= tail.size()) {
        if (error) {
          *error = iniError(
              lineNumber,
              "section must be in form [audio-filter.<preset>.<section>]");
        }
        return false;
      }

      presetName = trimIniToken(tail.substr(0, separator));
      if (presetName.empty()) {
        if (error) {
          *error = iniError(lineNumber, "audio-filter preset name is empty");
        }
        return false;
      }

      section = normalizeIniName(trimIniToken(tail.substr(separator + 1)));
      if (section.empty() || !isKnownRadioSection(section)) {
        if (error) {
          *error = iniError(
              lineNumber,
              "unknown audio-filter section '" + tail.substr(separator + 1) +
                  "' in section header");
        }
        return false;
      }
      continue;
    }

      size_t separator = line.find('=');
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

      std::string value = trimIniToken(line.substr(separator + 1));
      if (value.size() >= 2) {
        if ((value.front() == '"' && value.back() == '"') ||
            (value.front() == '\'' && value.back() == '\'')) {
          value = value.substr(1, value.size() - 2);
        }
      }
      if (presetName.empty() || section.empty()) {
        if (error) {
          *error = iniError(lineNumber,
                          "setting outside section, expected "
                          "[audio-filter.<preset>.<section>]");
      }
      return false;
    }

    presets[presetName][section].push_back({lineNumber, key, value});
  }

  if (settingsFile.bad()) {
    if (error) {
      *error = "Failed while reading radio settings file: " + path;
    }
    return false;
  }
  return true;
}

static bool selectAudioFilterPreset(const IniAudioFilterConfig& presets,
                                   const std::string& path,
                                   const std::string& presetName,
                                   const IniAudioFilterPreset** selectedPreset,
                                   std::string& selectedPresetName,
                                   std::string* error) {
  if (presets.empty()) {
    if (error) {
      *error = "audio-filters has no presets.";
    }
    return false;
  }

  const std::string requestedNorm = normalizeIniName(presetName);
  const IniAudioFilterPreset* selected = nullptr;
  if (!presetName.empty()) {
    for (const auto& presetPair : presets) {
      if (normalizeIniName(presetPair.first) == requestedNorm) {
        selected = &presetPair.second;
        selectedPresetName = presetPair.first;
        break;
      }
    }
    if (!selected) {
      std::string available;
      for (const auto& presetPair : presets) {
        if (!available.empty()) available += ", ";
        available += presetPair.first;
      }
      if (error) {
        *error = "Unknown audio filter preset '" + presetName +
                 "' in '" + path + "'. Available presets: " +
                 available;
      }
      return false;
    }
    *selectedPreset = selected;
    return true;
  }

  for (const auto& presetPair : presets) {
    if (normalizeIniName(presetPair.first) == "default") {
      selected = &presetPair.second;
      selectedPresetName = presetPair.first;
      break;
    }
  }
  if (selected) {
    *selectedPreset = selected;
    return true;
  }

  if (presets.size() != 1) {
    std::string available;
    for (const auto& presetPair : presets) {
      if (!available.empty()) available += ", ";
      available += presetPair.first;
    }
    if (error) {
      *error = "audio-filters has multiple presets; supply --radio-preset from: " +
               available;
    }
    return false;
  }

  auto singleIt = presets.begin();
  *selectedPreset = &singleIt->second;
  selectedPresetName = singleIt->first;
  return true;
}

static bool applyRadioSettingsFromIniFile(Radio1938& radio,
                                         const std::string& path,
                                         const std::string& presetName,
                                         std::string* error) {
  if (path.empty()) {
    if (error) *error = "Radio settings path is empty.";
    return false;
  }

  IniAudioFilterConfig presets;
  if (!parseAudioFilterIniSettings(path, presets, error)) {
    return false;
  }

  const IniAudioFilterPreset* selectedPreset = nullptr;
  std::string selectedPresetName;
  if (!selectAudioFilterPreset(presets, path, presetName, &selectedPreset,
                              selectedPresetName, error)) {
    return false;
  }

  for (const auto& sectionPair : *selectedPreset) {
    const std::string& sectionName = sectionPair.first;
    for (const auto& setting : sectionPair.second) {
      if (!applyRadioSettingsValue(radio,
                                   sectionName,
                                   normalizeIniName(setting.key),
                                   setting.value,
                                   setting.line,
                                   error)) {
        return false;
      }
    }
  }
  return true;
}

bool applyRadioSettingsIni(Radio1938& radio,
                          const std::string& path,
                          const std::string& presetName,
                          std::string* error) {
  if (path.empty()) {
    if (error) *error = "Radio settings path is empty.";
    return false;
  }
  if (!endsWithIgnoreCase(path, ".toml")) {
    if (error) {
      *error = "Radio settings file must be a .toml file: " + path;
    }
    return false;
  }
  bool wasInitialized = radio.initialized;
  int channels = radio.channels;
  float sampleRate = radio.sampleRate;
  float bwHz = radio.bwHz;
  float noiseWeight = radio.noiseWeight;
  if (!applyRadioSettingsFromIniFile(radio, path, presetName, error)) {
    return false;
  }
  if (wasInitialized) {
    radio.init(channels, sampleRate, bwHz, noiseWeight);
  }
  return true;
}
