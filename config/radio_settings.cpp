#include "radio.h"

#include <algorithm>
#include <array>
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

struct RadioSettingEntry {
  const char* key = nullptr;
  bool (*apply)(Radio1938& radio,
                const std::string& valueText,
                int lineNumber,
                std::string* error) = nullptr;
};

struct RadioSettingSection {
  const char* name = nullptr;
  const char* unknownLabel = "setting";
  const RadioSettingEntry* settings = nullptr;
  size_t settingCount = 0;
  bool unknownIncludesSection = true;
};

template <size_t N>
static RadioSettingSection makeRadioSettingSection(
    const char* name,
    const std::array<RadioSettingEntry, N>& settings,
    const char* unknownLabel = "setting",
    bool unknownIncludesSection = true) {
  return RadioSettingSection{name, unknownLabel, settings.data(), N,
                             unknownIncludesSection};
}

static bool applyIniFloatSetting(const std::string& valueText,
                                 float& target,
                                 std::string* error,
                                 int lineNumber,
                                 const char* settingName) {
  float parsed = 0.0f;
  if (!parseIniFloat(valueText, parsed, error, lineNumber, settingName)) {
    return false;
  }
  target = parsed;
  return true;
}

static bool applyIniIntSetting(const std::string& valueText,
                               int& target,
                               std::string* error,
                               int lineNumber,
                               const char* settingName) {
  int parsed = 0;
  if (!parseIniInt(valueText, parsed, error, lineNumber, settingName)) {
    return false;
  }
  target = parsed;
  return true;
}

static bool applyIniBoolSetting(const std::string& valueText,
                                bool& target,
                                std::string* error,
                                int lineNumber,
                                const char* settingName) {
  bool parsed = false;
  if (!parseIniBool(valueText, parsed, error, lineNumber, settingName)) {
    return false;
  }
  target = parsed;
  return true;
}

#define RADIO_FLOAT_ENTRY(keyLiteral, targetExpr, settingLiteral)                  \
  RadioSettingEntry{                                                               \
      keyLiteral,                                                                  \
      +[](Radio1938& radio,                                                        \
           const std::string& valueText,                                           \
           int lineNumber,                                                         \
           std::string* error) {                                                   \
        return applyIniFloatSetting(valueText, targetExpr, error, lineNumber,      \
                                    settingLiteral);                               \
      }}

#define RADIO_INT_ENTRY(keyLiteral, targetExpr, settingLiteral)                    \
  RadioSettingEntry{                                                               \
      keyLiteral,                                                                  \
      +[](Radio1938& radio,                                                        \
           const std::string& valueText,                                           \
           int lineNumber,                                                         \
           std::string* error) {                                                   \
        return applyIniIntSetting(valueText, targetExpr, error, lineNumber,        \
                                  settingLiteral);                                 \
      }}

#define RADIO_BOOL_ENTRY(keyLiteral, targetExpr, settingLiteral)                   \
  RadioSettingEntry{                                                               \
      keyLiteral,                                                                  \
      +[](Radio1938& radio,                                                        \
           const std::string& valueText,                                           \
           int lineNumber,                                                         \
           std::string* error) {                                                   \
        return applyIniBoolSetting(valueText, targetExpr, error, lineNumber,       \
                                   settingLiteral);                                \
      }}

#define RADIO_BOOL_CUSTOM_ENTRY(keyLiteral, settingLiteral, assignStmt)            \
  RadioSettingEntry{                                                               \
      keyLiteral,                                                                  \
      +[](Radio1938& radio,                                                        \
           const std::string& valueText,                                           \
           int lineNumber,                                                         \
           std::string* error) {                                                   \
        bool parsed = false;                                                       \
        if (!parseIniBool(valueText, parsed, error, lineNumber, settingLiteral)) { \
          return false;                                                            \
        }                                                                          \
        assignStmt;                                                                \
        return true;                                                               \
      }}


static const std::array<RadioSettingEntry, 10> kGlobalsSettings = {{
    RADIO_FLOAT_ENTRY("ifnoisemix", radio.globals.ifNoiseMix, "globals.ifNoiseMix"),
    RADIO_FLOAT_ENTRY("inputpad", radio.globals.inputPad, "globals.inputPad"),
    RADIO_BOOL_ENTRY("enableautolevel", radio.globals.enableAutoLevel, "globals.enableAutoLevel"),
    RADIO_FLOAT_ENTRY("autotargetdb", radio.globals.autoTargetDb, "globals.autoTargetDb"),
    RADIO_FLOAT_ENTRY("automaxboostdb", radio.globals.autoMaxBoostDb, "globals.autoMaxBoostDb"),
    RADIO_FLOAT_ENTRY("outputclipthreshold", radio.globals.outputClipThreshold, "globals.outputClipThreshold"),
    RADIO_FLOAT_ENTRY("oversamplefactor", radio.globals.oversampleFactor, "globals.oversampleFactor"),
    RADIO_FLOAT_ENTRY("oversamplecutofffraction", radio.globals.oversampleCutoffFraction, "globals.oversampleCutoffFraction"),
    RADIO_FLOAT_ENTRY("postnoisemix", radio.globals.postNoiseMix, "globals.postNoiseMix"),
    RADIO_FLOAT_ENTRY("noiseflooramp", radio.globals.noiseFloorAmp, "globals.noiseFloorAmp"),
}};

static const std::array<RadioSettingEntry, 12> kTuningSettings = {{
    RADIO_FLOAT_ENTRY("tuneoffsethz", radio.tuning.tuneOffsetHz, "tuning.tuneOffsetHz"),
    RADIO_FLOAT_ENTRY("safebwminhz", radio.tuning.safeBwMinHz, "tuning.safeBwMinHz"),
    RADIO_FLOAT_ENTRY("safebwmaxhz", radio.tuning.safeBwMaxHz, "tuning.safeBwMaxHz"),
    RADIO_FLOAT_ENTRY("prebwscale", radio.tuning.preBwScale, "tuning.preBwScale"),
    RADIO_FLOAT_ENTRY("postbwscale", radio.tuning.postBwScale, "tuning.postBwScale"),
    RADIO_FLOAT_ENTRY("smoothtau", radio.tuning.smoothTau, "tuning.smoothTau"),
    RADIO_FLOAT_ENTRY("updateeps", radio.tuning.updateEps, "tuning.updateEps"),
    RADIO_BOOL_ENTRY("magnetictuningenabled", radio.tuning.magneticTuningEnabled, "tuning.magneticTuningEnabled"),
    RADIO_FLOAT_ENTRY("afccapturehz", radio.tuning.afcCaptureHz, "tuning.afcCaptureHz"),
    RADIO_FLOAT_ENTRY("afcmaxcorrectionhz", radio.tuning.afcMaxCorrectionHz, "tuning.afcMaxCorrectionHz"),
    RADIO_FLOAT_ENTRY("afcdeadband", radio.tuning.afcDeadband, "tuning.afcDeadband"),
    RADIO_FLOAT_ENTRY("afcresponsems", radio.tuning.afcResponseMs, "tuning.afcResponseMs"),
}};

static const std::array<RadioSettingEntry, 14> kFrontEndSettings = {{
    RADIO_FLOAT_ENTRY("inputhphz", radio.frontEnd.inputHpHz, "frontEnd.inputHpHz"),
    RADIO_FLOAT_ENTRY("rfgain", radio.frontEnd.rfGain, "frontEnd.rfGain"),
    RADIO_FLOAT_ENTRY("avcgaindepth", radio.frontEnd.avcGainDepth, "frontEnd.avcGainDepth"),
    RADIO_FLOAT_ENTRY("selectivitypeakhz", radio.frontEnd.selectivityPeakHz, "frontEnd.selectivityPeakHz"),
    RADIO_FLOAT_ENTRY("selectivitypeakq", radio.frontEnd.selectivityPeakQ, "frontEnd.selectivityPeakQ"),
    RADIO_FLOAT_ENTRY("selectivitypeakgaindb", radio.frontEnd.selectivityPeakGainDb, "frontEnd.selectivityPeakGainDb"),
    RADIO_FLOAT_ENTRY("antennainductancehenries", radio.frontEnd.antennaInductanceHenries, "frontEnd.antennaInductanceHenries"),
    RADIO_FLOAT_ENTRY("antennacapacitancefarads", radio.frontEnd.antennaCapacitanceFarads, "frontEnd.antennaCapacitanceFarads"),
    RADIO_FLOAT_ENTRY("antennaseriesresistanceohms", radio.frontEnd.antennaSeriesResistanceOhms, "frontEnd.antennaSeriesResistanceOhms"),
    RADIO_FLOAT_ENTRY("antennaloadresistanceohms", radio.frontEnd.antennaLoadResistanceOhms, "frontEnd.antennaLoadResistanceOhms"),
    RADIO_FLOAT_ENTRY("rfinductancehenries", radio.frontEnd.rfInductanceHenries, "frontEnd.rfInductanceHenries"),
    RADIO_FLOAT_ENTRY("rfcapacitancefarads", radio.frontEnd.rfCapacitanceFarads, "frontEnd.rfCapacitanceFarads"),
    RADIO_FLOAT_ENTRY("rfseriesresistanceohms", radio.frontEnd.rfSeriesResistanceOhms, "frontEnd.rfSeriesResistanceOhms"),
    RADIO_FLOAT_ENTRY("rfloadresistanceohms", radio.frontEnd.rfLoadResistanceOhms, "frontEnd.rfLoadResistanceOhms"),
}};

static const std::array<RadioSettingEntry, 19> kMixerSettings = {{
    RADIO_FLOAT_ENTRY("rfgriddrivevolts", radio.mixer.rfGridDriveVolts, "mixer.rfGridDriveVolts"),
    RADIO_FLOAT_ENTRY("logriddrivevolts", radio.mixer.loGridDriveVolts, "mixer.loGridDriveVolts"),
    RADIO_FLOAT_ENTRY("logridbiasvolts", radio.mixer.loGridBiasVolts, "mixer.loGridBiasVolts"),
    RADIO_FLOAT_ENTRY("avcgriddrivevolts", radio.mixer.avcGridDriveVolts, "mixer.avcGridDriveVolts"),
    RADIO_FLOAT_ENTRY("platesupplyvolts", radio.mixer.plateSupplyVolts, "mixer.plateSupplyVolts"),
    RADIO_FLOAT_ENTRY("platedcvolts", radio.mixer.plateDcVolts, "mixer.plateDcVolts"),
    RADIO_FLOAT_ENTRY("platequiescentvolts", radio.mixer.plateQuiescentVolts, "mixer.plateQuiescentVolts"),
    RADIO_FLOAT_ENTRY("screenvolts", radio.mixer.screenVolts, "mixer.screenVolts"),
    RADIO_FLOAT_ENTRY("biasvolts", radio.mixer.biasVolts, "mixer.biasVolts"),
    RADIO_FLOAT_ENTRY("cutoffvolts", radio.mixer.cutoffVolts, "mixer.cutoffVolts"),
    RADIO_FLOAT_ENTRY("modelcutoffvolts", radio.mixer.modelCutoffVolts, "mixer.modelCutoffVolts"),
    RADIO_FLOAT_ENTRY("platecurrentamps", radio.mixer.plateCurrentAmps, "mixer.plateCurrentAmps"),
    RADIO_FLOAT_ENTRY("platequiescentcurrentamps", radio.mixer.plateQuiescentCurrentAmps, "mixer.plateQuiescentCurrentAmps"),
    RADIO_FLOAT_ENTRY("mutualconductancesiemens", radio.mixer.mutualConductanceSiemens, "mixer.mutualConductanceSiemens"),
    RADIO_FLOAT_ENTRY("acloadresistanceohms", radio.mixer.acLoadResistanceOhms, "mixer.acLoadResistanceOhms"),
    RADIO_FLOAT_ENTRY("platekneevolts", radio.mixer.plateKneeVolts, "mixer.plateKneeVolts"),
    RADIO_FLOAT_ENTRY("gridsoftnessvolts", radio.mixer.gridSoftnessVolts, "mixer.gridSoftnessVolts"),
    RADIO_FLOAT_ENTRY("plateresistanceohms", radio.mixer.plateResistanceOhms, "mixer.plateResistanceOhms"),
    RADIO_FLOAT_ENTRY("operatingpointtolerancevolts", radio.mixer.operatingPointToleranceVolts, "mixer.operatingPointToleranceVolts"),
}};

static const std::array<RadioSettingEntry, 10> kIfStripSettings = {{
    RADIO_BOOL_ENTRY("enabled", radio.ifStrip.enabled, "ifStrip.enabled"),
    RADIO_FLOAT_ENTRY("ifminbwhz", radio.ifStrip.ifMinBwHz, "ifStrip.ifMinBwHz"),
    RADIO_FLOAT_ENTRY("stagegain", radio.ifStrip.stageGain, "ifStrip.stageGain"),
    RADIO_FLOAT_ENTRY("avcgaindepth", radio.ifStrip.avcGainDepth, "ifStrip.avcGainDepth"),
    RADIO_FLOAT_ENTRY("ifcenterhz", radio.ifStrip.ifCenterHz, "ifStrip.ifCenterHz"),
    RADIO_FLOAT_ENTRY("primaryinductancehenries", radio.ifStrip.primaryInductanceHenries, "ifStrip.primaryInductanceHenries"),
    RADIO_FLOAT_ENTRY("secondaryinductancehenries", radio.ifStrip.secondaryInductanceHenries, "ifStrip.secondaryInductanceHenries"),
    RADIO_FLOAT_ENTRY("secondaryloadresistanceohms", radio.ifStrip.secondaryLoadResistanceOhms, "ifStrip.secondaryLoadResistanceOhms"),
    RADIO_FLOAT_ENTRY("interstagecouplingcoeff", radio.ifStrip.interstageCouplingCoeff, "ifStrip.interstageCouplingCoeff"),
    RADIO_FLOAT_ENTRY("outputcouplingcoeff", radio.ifStrip.outputCouplingCoeff, "ifStrip.outputCouplingCoeff"),
}};

static const std::array<RadioSettingEntry, 20> kDemodSettings = {{
    RADIO_FLOAT_ENTRY("audiodiodedrop", radio.demod.am.audioDiodeDrop, "demod.audioDiodeDrop"),
    RADIO_FLOAT_ENTRY("avcdiodedrop", radio.demod.am.avcDiodeDrop, "demod.avcDiodeDrop"),
    RADIO_FLOAT_ENTRY("audiojunctionslopevolts", radio.demod.am.audioJunctionSlopeVolts, "demod.audioJunctionSlopeVolts"),
    RADIO_FLOAT_ENTRY("avcjunctionslopevolts", radio.demod.am.avcJunctionSlopeVolts, "demod.avcJunctionSlopeVolts"),
    RADIO_FLOAT_ENTRY("detectorstoragecapfarads", radio.demod.am.detectorStorageCapFarads, "demod.detectorStorageCapFarads"),
    RADIO_FLOAT_ENTRY("audiochargeresistanceohms", radio.demod.am.audioChargeResistanceOhms, "demod.audioChargeResistanceOhms"),
    RADIO_FLOAT_ENTRY("audiodischargeresistanceohms", radio.demod.am.audioDischargeResistanceOhms, "demod.audioDischargeResistanceOhms"),
    RADIO_FLOAT_ENTRY("avcchargeresistanceohms", radio.demod.am.avcChargeResistanceOhms, "demod.avcChargeResistanceOhms"),
    RADIO_FLOAT_ENTRY("avcdischargeresistanceohms", radio.demod.am.avcDischargeResistanceOhms, "demod.avcDischargeResistanceOhms"),
    RADIO_FLOAT_ENTRY("avcfiltercapfarads", radio.demod.am.avcFilterCapFarads, "demod.avcFilterCapFarads"),
    RADIO_FLOAT_ENTRY("controlvoltageref", radio.demod.am.controlVoltageRef, "demod.controlVoltageRef"),
    RADIO_FLOAT_ENTRY("senselowhz", radio.demod.am.senseLowHz, "demod.senseLowHz"),
    RADIO_FLOAT_ENTRY("sensehighhz", radio.demod.am.senseHighHz, "demod.senseHighHz"),
    RADIO_FLOAT_ENTRY("afcsenselphz", radio.demod.am.afcSenseLpHz, "demod.afcSenseLpHz"),
    RADIO_FLOAT_ENTRY("afcsoffsethz", radio.demod.am.afcLowOffsetHz, "demod.afcLowOffsetHz"),
    RADIO_FLOAT_ENTRY("afchoffsethz", radio.demod.am.afcHighOffsetHz, "demod.afcHighOffsetHz"),
    RADIO_FLOAT_ENTRY("afclowstep", radio.demod.am.afcLowStep, "demod.afcLowStep"),
    RADIO_FLOAT_ENTRY("afchighstep", radio.demod.am.afcHighStep, "demod.afcHighStep"),
    RADIO_FLOAT_ENTRY("afclowphase", radio.demod.am.afcLowPhase, "demod.afcLowPhase"),
    RADIO_FLOAT_ENTRY("afchighphase", radio.demod.am.afcHighPhase, "demod.afcHighPhase"),
}};

static const std::array<RadioSettingEntry, 25> kReceiverCircuitSettings = {{
    RADIO_BOOL_ENTRY("enabled", radio.receiverCircuit.enabled, "receiverCircuit.enabled"),
    RADIO_FLOAT_ENTRY("volumecontrolresistanceohms", radio.receiverCircuit.volumeControlResistanceOhms, "receiverCircuit.volumeControlResistanceOhms"),
    RADIO_FLOAT_ENTRY("volumecontroltapresistanceohms", radio.receiverCircuit.volumeControlTapResistanceOhms, "receiverCircuit.volumeControlTapResistanceOhms"),
    RADIO_FLOAT_ENTRY("volumecontrolposition", radio.receiverCircuit.volumeControlPosition, "receiverCircuit.volumeControlPosition"),
    RADIO_FLOAT_ENTRY("volumecontrolloudnessresistanceohms", radio.receiverCircuit.volumeControlLoudnessResistanceOhms, "receiverCircuit.volumeControlLoudnessResistanceOhms"),
    RADIO_FLOAT_ENTRY("volumecontrolloudnesscapfarads", radio.receiverCircuit.volumeControlLoudnessCapFarads, "receiverCircuit.volumeControlLoudnessCapFarads"),
    RADIO_FLOAT_ENTRY("volumecontroltapvoltage", radio.receiverCircuit.volumeControlTapVoltage, "receiverCircuit.volumeControlTapVoltage"),
    RADIO_FLOAT_ENTRY("couplingcapfarads", radio.receiverCircuit.couplingCapFarads, "receiverCircuit.couplingCapFarads"),
    RADIO_FLOAT_ENTRY("gridleakresistanceohms", radio.receiverCircuit.gridLeakResistanceOhms, "receiverCircuit.gridLeakResistanceOhms"),
    RADIO_FLOAT_ENTRY("tubeplatesupplyvolts", radio.receiverCircuit.tubePlateSupplyVolts, "receiverCircuit.tubePlateSupplyVolts"),
    RADIO_FLOAT_ENTRY("tubeplatedcvolts", radio.receiverCircuit.tubePlateDcVolts, "receiverCircuit.tubePlateDcVolts"),
    RADIO_FLOAT_ENTRY("tubequiescentplatevolts", radio.receiverCircuit.tubeQuiescentPlateVolts, "receiverCircuit.tubeQuiescentPlateVolts"),
    RADIO_FLOAT_ENTRY("tubescreenvolts", radio.receiverCircuit.tubeScreenVolts, "receiverCircuit.tubeScreenVolts"),
    RADIO_FLOAT_ENTRY("tubebiasvolts", radio.receiverCircuit.tubeBiasVolts, "receiverCircuit.tubeBiasVolts"),
    RADIO_FLOAT_ENTRY("tubeplatecurrentamps", radio.receiverCircuit.tubePlateCurrentAmps, "receiverCircuit.tubePlateCurrentAmps"),
    RADIO_FLOAT_ENTRY("tubequiescentplatecurrentamps", radio.receiverCircuit.tubeQuiescentPlateCurrentAmps, "receiverCircuit.tubeQuiescentPlateCurrentAmps"),
    RADIO_FLOAT_ENTRY("tubemutualconductancesiemens", radio.receiverCircuit.tubeMutualConductanceSiemens, "receiverCircuit.tubeMutualConductanceSiemens"),
    RADIO_FLOAT_ENTRY("tubemu", radio.receiverCircuit.tubeMu, "receiverCircuit.tubeMu"),
    RADIO_BOOL_ENTRY("tubetriodeconnected", radio.receiverCircuit.tubeTriodeConnected, "receiverCircuit.tubeTriodeConnected"),
    RADIO_FLOAT_ENTRY("tubeloadresistanceohms", radio.receiverCircuit.tubeLoadResistanceOhms, "receiverCircuit.tubeLoadResistanceOhms"),
    RADIO_FLOAT_ENTRY("tubeplatekneevolts", radio.receiverCircuit.tubePlateKneeVolts, "receiverCircuit.tubePlateKneeVolts"),
    RADIO_FLOAT_ENTRY("tubegridsoftnessvolts", radio.receiverCircuit.tubeGridSoftnessVolts, "receiverCircuit.tubeGridSoftnessVolts"),
    RADIO_FLOAT_ENTRY("tubeplatresistanceohms", radio.receiverCircuit.tubePlateResistanceOhms, "receiverCircuit.tubePlateResistanceOhms"),
    RADIO_FLOAT_ENTRY("operatingpointtolerancevolts", radio.receiverCircuit.operatingPointToleranceVolts, "receiverCircuit.operatingPointToleranceVolts"),
    RADIO_FLOAT_ENTRY("tubeplatevoltage", radio.receiverCircuit.tubePlateVoltage, "receiverCircuit.tubePlateVoltage"),
}};

static const std::array<RadioSettingEntry, 5> kToneSettings = {{
    RADIO_FLOAT_ENTRY("presencehz", radio.tone.presenceHz, "tone.presenceHz"),
    RADIO_FLOAT_ENTRY("presenceq", radio.tone.presenceQ, "tone.presenceQ"),
    RADIO_FLOAT_ENTRY("presencegaindb", radio.tone.presenceGainDb, "tone.presenceGainDb"),
    RADIO_FLOAT_ENTRY("tiltsplithz", radio.tone.tiltSplitHz, "tone.tiltSplitHz"),
    RADIO_FLOAT_ENTRY("tiltdepthdb", radio.tone.tiltDepthDb, "tone.tiltDepthDb"),
}};

static const std::array<RadioSettingEntry, 79> kPowerSettings = {{
    RADIO_FLOAT_ENTRY("sagenv", radio.power.sagEnv, "power.sagEnv"),
    RADIO_FLOAT_ENTRY("sagatk", radio.power.sagAtk, "power.sagAtk"),
    RADIO_FLOAT_ENTRY("sagrel", radio.power.sagRel, "power.sagRel"),
    RADIO_FLOAT_ENTRY("sagstart", radio.power.sagStart, "power.sagStart"),
    RADIO_FLOAT_ENTRY("sagend", radio.power.sagEnd, "power.sagEnd"),
    RADIO_FLOAT_ENTRY("rectifierphase", radio.power.rectifierPhase, "power.rectifierPhase"),
    RADIO_FLOAT_ENTRY("rippledepth", radio.power.rippleDepth, "power.rippleDepth"),
    RADIO_FLOAT_ENTRY("sagattackms", radio.power.sagAttackMs, "power.sagAttackMs"),
    RADIO_FLOAT_ENTRY("sagreleasems", radio.power.sagReleaseMs, "power.sagReleaseMs"),
    RADIO_FLOAT_ENTRY("rectifierminhz", radio.power.rectifierMinHz, "power.rectifierMinHz"),
    RADIO_FLOAT_ENTRY("ripplesecondharmonicmix", radio.power.rippleSecondHarmonicMix, "power.rippleSecondHarmonicMix"),
    RADIO_FLOAT_ENTRY("rippelesecondharmonicmix", radio.power.rippleSecondHarmonicMix, "power.rippleSecondHarmonicMix"),
    RADIO_FLOAT_ENTRY("gainsagperpower", radio.power.gainSagPerPower, "power.gainSagPerPower"),
    RADIO_FLOAT_ENTRY("ripplegainbase", radio.power.rippleGainBase, "power.rippleGainBase"),
    RADIO_FLOAT_ENTRY("ripplegaindepth", radio.power.rippleGainDepth, "power.rippleGainDepth"),
    RADIO_FLOAT_ENTRY("gainmin", radio.power.gainMin, "power.gainMin"),
    RADIO_FLOAT_ENTRY("gainmax", radio.power.gainMax, "power.gainMax"),
    RADIO_FLOAT_ENTRY("supplydrivedepth", radio.power.supplyDriveDepth, "power.supplyDriveDepth"),
    RADIO_FLOAT_ENTRY("supplybiasdepth", radio.power.supplyBiasDepth, "power.supplyBiasDepth"),
    RADIO_FLOAT_ENTRY("postlphz", radio.power.postLpHz, "power.postLpHz"),
    RADIO_FLOAT_ENTRY("gridcouplingcapfarads", radio.power.gridCouplingCapFarads, "power.gridCouplingCapFarads"),
    RADIO_FLOAT_ENTRY("gridleakresistanceohms", radio.power.gridLeakResistanceOhms, "power.gridLeakResistanceOhms"),
    RADIO_FLOAT_ENTRY("driversourceresistanceohms", radio.power.driverSourceResistanceOhms, "power.driverSourceResistanceOhms"),
    RADIO_FLOAT_ENTRY("tubeplatesupplyvolts", radio.power.tubePlateSupplyVolts, "power.tubePlateSupplyVolts"),
    RADIO_FLOAT_ENTRY("tubeplatedcvolts", radio.power.tubePlateDcVolts, "power.tubePlateDcVolts"),
    RADIO_FLOAT_ENTRY("tubequiescentplatevolts", radio.power.tubeQuiescentPlateVolts, "power.tubeQuiescentPlateVolts"),
    RADIO_FLOAT_ENTRY("tubescreenvolts", radio.power.tubeScreenVolts, "power.tubeScreenVolts"),
    RADIO_FLOAT_ENTRY("tubebiasvolts", radio.power.tubeBiasVolts, "power.tubeBiasVolts"),
    RADIO_FLOAT_ENTRY("tubeplatecurrentamps", radio.power.tubePlateCurrentAmps, "power.tubePlateCurrentAmps"),
    RADIO_FLOAT_ENTRY("tubequiescentplatecurrentamps", radio.power.tubeQuiescentPlateCurrentAmps, "power.tubeQuiescentPlateCurrentAmps"),
    RADIO_FLOAT_ENTRY("tubemutualconductancesiemens", radio.power.tubeMutualConductanceSiemens, "power.tubeMutualConductanceSiemens"),
    RADIO_FLOAT_ENTRY("tubemu", radio.power.tubeMu, "power.tubeMu"),
    RADIO_BOOL_ENTRY("tubetriodeconnected", radio.power.tubeTriodeConnected, "power.tubeTriodeConnected"),
    RADIO_FLOAT_ENTRY("tubeacloadresistanceohms", radio.power.tubeAcLoadResistanceOhms, "power.tubeAcLoadResistanceOhms"),
    RADIO_FLOAT_ENTRY("tubeplatekneevolts", radio.power.tubePlateKneeVolts, "power.tubePlateKneeVolts"),
    RADIO_FLOAT_ENTRY("tubegridsoftnessvolts", radio.power.tubeGridSoftnessVolts, "power.tubeGridSoftnessVolts"),
    RADIO_FLOAT_ENTRY("tubegridcurrentresistanceohms", radio.power.tubeGridCurrentResistanceOhms, "power.tubeGridCurrentResistanceOhms"),
    RADIO_FLOAT_ENTRY("tubeplatresistanceohms", radio.power.tubePlateResistanceOhms, "power.tubePlateResistanceOhms"),
    RADIO_FLOAT_ENTRY("operatingpointtolerancevolts", radio.power.operatingPointToleranceVolts, "power.operatingPointToleranceVolts"),
    RADIO_FLOAT_ENTRY("tubeplatevoltage", radio.power.tubePlateVoltage, "power.tubePlateVoltage"),
    RADIO_FLOAT_ENTRY("interstageprimaryleakageinductancehenries", radio.power.interstagePrimaryLeakageInductanceHenries, "power.interstagePrimaryLeakageInductanceHenries"),
    RADIO_FLOAT_ENTRY("interstagemagnetizinginductancehenries", radio.power.interstageMagnetizingInductanceHenries, "power.interstageMagnetizingInductanceHenries"),
    RADIO_FLOAT_ENTRY("interstageprimaryresistanceohms", radio.power.interstagePrimaryResistanceOhms, "power.interstagePrimaryResistanceOhms"),
    RADIO_FLOAT_ENTRY("interstageprimarycorelossresistanceohms", radio.power.interstagePrimaryCoreLossResistanceOhms, "power.interstagePrimaryCoreLossResistanceOhms"),
    RADIO_FLOAT_ENTRY("interstageturnsratioprimarytosecondary", radio.power.interstageTurnsRatioPrimaryToSecondary, "power.interstageTurnsRatioPrimaryToSecondary"),
    RADIO_FLOAT_ENTRY("interstageprimaryshuntcapfarads", radio.power.interstagePrimaryShuntCapFarads, "power.interstagePrimaryShuntCapFarads"),
    RADIO_FLOAT_ENTRY("interstagesecondaryleakageinductancehenries", radio.power.interstageSecondaryLeakageInductanceHenries, "power.interstageSecondaryLeakageInductanceHenries"),
    RADIO_FLOAT_ENTRY("interstagesecondaryresistanceohms", radio.power.interstageSecondaryResistanceOhms, "power.interstageSecondaryResistanceOhms"),
    RADIO_FLOAT_ENTRY("interstagesecondaryshuntcapfarads", radio.power.interstageSecondaryShuntCapFarads, "power.interstageSecondaryShuntCapFarads"),
    RADIO_INT_ENTRY("interstageintegrationsubsteps", radio.power.interstageIntegrationSubsteps, "power.interstageIntegrationSubsteps"),
    RADIO_FLOAT_ENTRY("outputgridleakresistanceohms", radio.power.outputGridLeakResistanceOhms, "power.outputGridLeakResistanceOhms"),
    RADIO_FLOAT_ENTRY("outputgridcurrentresistanceohms", radio.power.outputGridCurrentResistanceOhms, "power.outputGridCurrentResistanceOhms"),
    RADIO_FLOAT_ENTRY("outputtubeplatesupplyvolts", radio.power.outputTubePlateSupplyVolts, "power.outputTubePlateSupplyVolts"),
    RADIO_FLOAT_ENTRY("outputtubeplatedcvolts", radio.power.outputTubePlateDcVolts, "power.outputTubePlateDcVolts"),
    RADIO_FLOAT_ENTRY("outputtubequiescentplatevolts", radio.power.outputTubeQuiescentPlateVolts, "power.outputTubeQuiescentPlateVolts"),
    RADIO_FLOAT_ENTRY("outputtubebiasvolts", radio.power.outputTubeBiasVolts, "power.outputTubeBiasVolts"),
    RADIO_FLOAT_ENTRY("outputtubeplatecurrentamps", radio.power.outputTubePlateCurrentAmps, "power.outputTubePlateCurrentAmps"),
    RADIO_FLOAT_ENTRY("outputtubequiescentplatecurrentamps", radio.power.outputTubeQuiescentPlateCurrentAmps, "power.outputTubeQuiescentPlateCurrentAmps"),
    RADIO_FLOAT_ENTRY("outputtubemutualconductancesiemens", radio.power.outputTubeMutualConductanceSiemens, "power.outputTubeMutualConductanceSiemens"),
    RADIO_FLOAT_ENTRY("outputtubemu", radio.power.outputTubeMu, "power.outputTubeMu"),
    RADIO_FLOAT_ENTRY("outputtubeplatetoplateloadohms", radio.power.outputTubePlateToPlateLoadOhms, "power.outputTubePlateToPlateLoadOhms"),
    RADIO_FLOAT_ENTRY("outputtubeplatekneevolts", radio.power.outputTubePlateKneeVolts, "power.outputTubePlateKneeVolts"),
    RADIO_FLOAT_ENTRY("outputtubegridsoftnessvolts", radio.power.outputTubeGridSoftnessVolts, "power.outputTubeGridSoftnessVolts"),
    RADIO_FLOAT_ENTRY("outputtubeplatresistanceohms", radio.power.outputTubePlateResistanceOhms, "power.outputTubePlateResistanceOhms"),
    RADIO_FLOAT_ENTRY("outputgridavolts", radio.power.outputGridAVolts, "power.outputGridAVolts"),
    RADIO_FLOAT_ENTRY("outputgridbvolts", radio.power.outputGridBVolts, "power.outputGridBVolts"),
    RADIO_FLOAT_ENTRY("outputoperatingpointtolerancevolts", radio.power.outputOperatingPointToleranceVolts, "power.outputOperatingPointToleranceVolts"),
    RADIO_FLOAT_ENTRY("outputtransformerprimaryleakageinductancehenries", radio.power.outputTransformerPrimaryLeakageInductanceHenries, "power.outputTransformerPrimaryLeakageInductanceHenries"),
    RADIO_FLOAT_ENTRY("outputtransformermagnetizinginductancehenries", radio.power.outputTransformerMagnetizingInductanceHenries, "power.outputTransformerMagnetizingInductanceHenries"),
    RADIO_FLOAT_ENTRY("outputtransformerturnsratioprimarytosecondary", radio.power.outputTransformerTurnsRatioPrimaryToSecondary, "power.outputTransformerTurnsRatioPrimaryToSecondary"),
    RADIO_FLOAT_ENTRY("outputtransformerprimaryresistanceohms", radio.power.outputTransformerPrimaryResistanceOhms, "power.outputTransformerPrimaryResistanceOhms"),
    RADIO_FLOAT_ENTRY("outputtransformerprimarycorelossresistanceohms", radio.power.outputTransformerPrimaryCoreLossResistanceOhms, "power.outputTransformerPrimaryCoreLossResistanceOhms"),
    RADIO_FLOAT_ENTRY("outputtransformerprimaryshuntcapfarads", radio.power.outputTransformerPrimaryShuntCapFarads, "power.outputTransformerPrimaryShuntCapFarads"),
    RADIO_FLOAT_ENTRY("outputtransformersecondaryleakageinductancehenries", radio.power.outputTransformerSecondaryLeakageInductanceHenries, "power.outputTransformerSecondaryLeakageInductanceHenries"),
    RADIO_FLOAT_ENTRY("outputtransformersecondaryresistanceohms", radio.power.outputTransformerSecondaryResistanceOhms, "power.outputTransformerSecondaryResistanceOhms"),
    RADIO_FLOAT_ENTRY("outputtransformersecondaryshuntcapfarads", radio.power.outputTransformerSecondaryShuntCapFarads, "power.outputTransformerSecondaryShuntCapFarads"),
    RADIO_INT_ENTRY("outputtransformerintegrationsubsteps", radio.power.outputTransformerIntegrationSubsteps, "power.outputTransformerIntegrationSubsteps"),
    RADIO_FLOAT_ENTRY("outputloadresistanceohms", radio.power.outputLoadResistanceOhms, "power.outputLoadResistanceOhms"),
    RADIO_FLOAT_ENTRY("nominaloutputpowerwatts", radio.power.nominalOutputPowerWatts, "power.nominalOutputPowerWatts"),
}};

static const std::array<RadioSettingEntry, 32> kNoiseSettings = {{
    RADIO_BOOL_ENTRY("enablehumtone", radio.noiseConfig.enableHumTone, "noise.enableHumTone"),
    RADIO_FLOAT_ENTRY("humhzdefault", radio.noiseConfig.humHzDefault, "noise.humHzDefault"),
    RADIO_FLOAT_ENTRY("noiseref", radio.noiseConfig.noiseWeightRef, "noise.noiseWeightRef"),
    RADIO_FLOAT_ENTRY("noiseweightscalemax", radio.noiseConfig.noiseWeightScaleMax, "noise.noiseWeightScaleMax"),
    RADIO_FLOAT_ENTRY("humampscale", radio.noiseConfig.humAmpScale, "noise.humAmpScale"),
    RADIO_FLOAT_ENTRY("crackleampscale", radio.noiseConfig.crackleAmpScale, "noise.crackleAmpScale"),
    RADIO_FLOAT_ENTRY("crackleratescale", radio.noiseConfig.crackleRateScale, "noise.crackleRateScale"),
    RADIO_FLOAT_ENTRY("noisehphz", radio.noiseRuntime.hum.noiseHpHz, "noise.noiseHpHz"),
    RADIO_FLOAT_ENTRY("noiselphz", radio.noiseRuntime.hum.noiseLpHz, "noise.noiseLpHz"),
    RADIO_FLOAT_ENTRY("filterq", radio.noiseRuntime.hum.filterQ, "noise.filterQ"),
    RADIO_FLOAT_ENTRY("scattackms", radio.noiseRuntime.hum.scAttackMs, "noise.scAttackMs"),
    RADIO_FLOAT_ENTRY("screleasems", radio.noiseRuntime.hum.scReleaseMs, "noise.scReleaseMs"),
    RADIO_FLOAT_ENTRY("crackledecayms", radio.noiseRuntime.hum.crackleDecayMs, "noise.crackleDecayMs"),
    RADIO_FLOAT_ENTRY("sidechainmaskref", radio.noiseRuntime.hum.sidechainMaskRef, "noise.sidechainMaskRef"),
    RADIO_FLOAT_ENTRY("hissmaskdepth", radio.noiseRuntime.hum.hissMaskDepth, "noise.hissMaskDepth"),
    RADIO_FLOAT_ENTRY("burstmaskdepth", radio.noiseRuntime.hum.burstMaskDepth, "noise.burstMaskDepth"),
    RADIO_FLOAT_ENTRY("pinkfastpole", radio.noiseRuntime.hum.pinkFastPole, "noise.pinkFastPole"),
    RADIO_FLOAT_ENTRY("pinkslowpole", radio.noiseRuntime.hum.pinkSlowPole, "noise.pinkSlowPole"),
    RADIO_FLOAT_ENTRY("brownstep", radio.noiseRuntime.hum.brownStep, "noise.brownStep"),
    RADIO_FLOAT_ENTRY("hissdriftpole", radio.noiseRuntime.hum.hissDriftPole, "noise.hissDriftPole"),
    RADIO_FLOAT_ENTRY("hissdriftnoise", radio.noiseRuntime.hum.hissDriftNoise, "noise.hissDriftNoise"),
    RADIO_FLOAT_ENTRY("hissdriftslowpole", radio.noiseRuntime.hum.hissDriftSlowPole, "noise.hissDriftSlowPole"),
    RADIO_FLOAT_ENTRY("hissdriftslownoise", radio.noiseRuntime.hum.hissDriftSlowNoise, "noise.hissDriftSlowNoise"),
    RADIO_FLOAT_ENTRY("whitemix", radio.noiseRuntime.hum.whiteMix, "noise.whiteMix"),
    RADIO_FLOAT_ENTRY("pinkfastmix", radio.noiseRuntime.hum.pinkFastMix, "noise.pinkFastMix"),
    RADIO_FLOAT_ENTRY("pinkdifferencemix", radio.noiseRuntime.hum.pinkDifferenceMix, "noise.pinkDifferenceMix"),
    RADIO_FLOAT_ENTRY("pinkfastsubtract", radio.noiseRuntime.hum.pinkFastSubtract, "noise.pinkFastSubtract"),
    RADIO_FLOAT_ENTRY("brownmix", radio.noiseRuntime.hum.brownMix, "noise.brownMix"),
    RADIO_FLOAT_ENTRY("hissbase", radio.noiseRuntime.hum.hissBase, "noise.hissBase"),
    RADIO_FLOAT_ENTRY("hissdriftdepth", radio.noiseRuntime.hum.hissDriftDepth, "noise.hissDriftDepth"),
    RADIO_FLOAT_ENTRY("hissdriftslowmix", radio.noiseRuntime.hum.hissDriftSlowMix, "noise.hissDriftSlowMix"),
    RADIO_FLOAT_ENTRY("humsecondharmonicmix", radio.noiseRuntime.hum.humSecondHarmonicMix, "noise.humSecondHarmonicMix"),
}};

static const std::array<RadioSettingEntry, 33> kSpeakerSettings = {{
    RADIO_FLOAT_ENTRY("drive", radio.speakerStage.drive, "speaker.drive"),
    RADIO_FLOAT_ENTRY("suspensionhz", radio.speakerStage.speaker.suspensionHz, "speaker.suspensionHz"),
    RADIO_FLOAT_ENTRY("suspensionq", radio.speakerStage.speaker.suspensionQ, "speaker.suspensionQ"),
    RADIO_FLOAT_ENTRY("suspensiongaindb", radio.speakerStage.speaker.suspensionGainDb, "speaker.suspensionGainDb"),
    RADIO_FLOAT_ENTRY("conebodyhz", radio.speakerStage.speaker.coneBodyHz, "speaker.coneBodyHz"),
    RADIO_FLOAT_ENTRY("conebodyq", radio.speakerStage.speaker.coneBodyQ, "speaker.coneBodyQ"),
    RADIO_FLOAT_ENTRY("conebodygaindb", radio.speakerStage.speaker.coneBodyGainDb, "speaker.coneBodyGainDb"),
    RADIO_FLOAT_ENTRY("upperbreakuphz", radio.speakerStage.speaker.upperBreakupHz, "speaker.upperBreakupHz"),
    RADIO_FLOAT_ENTRY("upperbreakupq", radio.speakerStage.speaker.upperBreakupQ, "speaker.upperBreakupQ"),
    RADIO_FLOAT_ENTRY("upperbreakupgaindb", radio.speakerStage.speaker.upperBreakupGainDb, "speaker.upperBreakupGainDb"),
    RADIO_FLOAT_ENTRY("conediphz", radio.speakerStage.speaker.coneDipHz, "speaker.coneDipHz"),
    RADIO_FLOAT_ENTRY("conedipq", radio.speakerStage.speaker.coneDipQ, "speaker.coneDipQ"),
    RADIO_FLOAT_ENTRY("conedipgaindb", radio.speakerStage.speaker.coneDipGainDb, "speaker.coneDipGainDb"),
    RADIO_FLOAT_ENTRY("toplphz", radio.speakerStage.speaker.topLpHz, "speaker.topLpHz"),
    RADIO_FLOAT_ENTRY("hflosslphz", radio.speakerStage.speaker.hfLossLpHz, "speaker.hfLossLpHz"),
    RADIO_FLOAT_ENTRY("hflossdepth", radio.speakerStage.speaker.hfLossDepth, "speaker.hfLossDepth"),
    RADIO_FLOAT_ENTRY("filterq", radio.speakerStage.speaker.filterQ, "speaker.filterQ"),
    RADIO_FLOAT_ENTRY("limit", radio.speakerStage.speaker.limit, "speaker.limit"),
    RADIO_FLOAT_ENTRY("asymbias", radio.speakerStage.speaker.asymBias, "speaker.asymBias"),
    RADIO_FLOAT_ENTRY("suspensioncompliancetolerance", radio.speakerStage.speaker.suspensionComplianceTolerance, "speaker.suspensionComplianceTolerance"),
    RADIO_FLOAT_ENTRY("conemasstolerance", radio.speakerStage.speaker.coneMassTolerance, "speaker.coneMassTolerance"),
    RADIO_FLOAT_ENTRY("breakuptolerance", radio.speakerStage.speaker.breakupTolerance, "speaker.breakupTolerance"),
    RADIO_FLOAT_ENTRY("voicecoiltolerance", radio.speakerStage.speaker.voiceCoilTolerance, "speaker.voiceCoilTolerance"),
    RADIO_FLOAT_ENTRY("excursionref", radio.speakerStage.speaker.excursionRef, "speaker.excursionRef"),
    RADIO_FLOAT_ENTRY("compliancelossdepth", radio.speakerStage.speaker.complianceLossDepth, "speaker.complianceLossDepth"),
    RADIO_FLOAT_ENTRY("voicecoilresistanceohms", radio.speakerStage.speaker.voiceCoilResistanceOhms, "speaker.voiceCoilResistanceOhms"),
    RADIO_FLOAT_ENTRY("voicecoilinductancehenries", radio.speakerStage.speaker.voiceCoilInductanceHenries, "speaker.voiceCoilInductanceHenries"),
    RADIO_FLOAT_ENTRY("movingmasskg", radio.speakerStage.speaker.movingMassKg, "speaker.movingMassKg"),
    RADIO_FLOAT_ENTRY("mechanicalq", radio.speakerStage.speaker.mechanicalQ, "speaker.mechanicalQ"),
    RADIO_FLOAT_ENTRY("electricalq", radio.speakerStage.speaker.electricalQ, "speaker.electricalQ"),
    RADIO_FLOAT_ENTRY("forcefactorbl", radio.speakerStage.speaker.forceFactorBl, "speaker.forceFactorBl"),
    RADIO_FLOAT_ENTRY("suspensioncompliancemeterspernewton", radio.speakerStage.speaker.suspensionComplianceMetersPerNewton, "speaker.suspensionComplianceMetersPerNewton"),
    RADIO_FLOAT_ENTRY("mechanicaldampingnspermeter", radio.speakerStage.speaker.mechanicalDampingNsPerMeter, "speaker.mechanicalDampingNsPerMeter"),
}};

static const std::array<RadioSettingEntry, 15> kCabinetSettings = {{
    RADIO_BOOL_ENTRY("enabled", radio.cabinet.enabled, "cabinet.enabled"),
    RADIO_FLOAT_ENTRY("panelhz", radio.cabinet.panelHz, "cabinet.panelHz"),
    RADIO_FLOAT_ENTRY("panelq", radio.cabinet.panelQ, "cabinet.panelQ"),
    RADIO_FLOAT_ENTRY("panelgaindb", radio.cabinet.panelGainDb, "cabinet.panelGainDb"),
    RADIO_FLOAT_ENTRY("chassishz", radio.cabinet.chassisHz, "cabinet.chassisHz"),
    RADIO_FLOAT_ENTRY("chassisq", radio.cabinet.chassisQ, "cabinet.chassisQ"),
    RADIO_FLOAT_ENTRY("chassisgaindb", radio.cabinet.chassisGainDb, "cabinet.chassisGainDb"),
    RADIO_FLOAT_ENTRY("cavitydiphz", radio.cabinet.cavityDipHz, "cabinet.cavityDipHz"),
    RADIO_FLOAT_ENTRY("cavitydipq", radio.cabinet.cavityDipQ, "cabinet.cavityDipQ"),
    RADIO_FLOAT_ENTRY("cavitydipgaindb", radio.cabinet.cavityDipGainDb, "cabinet.cavityDipGainDb"),
    RADIO_FLOAT_ENTRY("grillelphz", radio.cabinet.grilleLpHz, "cabinet.grilleLpHz"),
    RADIO_FLOAT_ENTRY("reardelayms", radio.cabinet.rearDelayMs, "cabinet.rearDelayMs"),
    RADIO_FLOAT_ENTRY("rearmix", radio.cabinet.rearMix, "cabinet.rearMix"),
    RADIO_FLOAT_ENTRY("rearhphz", radio.cabinet.rearHpHz, "cabinet.rearHpHz"),
    RADIO_FLOAT_ENTRY("rearlphz", radio.cabinet.rearLpHz, "cabinet.rearLpHz"),
}};

static const std::array<RadioSettingEntry, 5> kFinalLimiterSettings = {{
    RADIO_BOOL_ENTRY("enabled", radio.finalLimiter.enabled, "finalLimiter.enabled"),
    RADIO_FLOAT_ENTRY("threshold", radio.finalLimiter.threshold, "finalLimiter.threshold"),
    RADIO_FLOAT_ENTRY("lookaheadms", radio.finalLimiter.lookaheadMs, "finalLimiter.lookaheadMs"),
    RADIO_FLOAT_ENTRY("attackms", radio.finalLimiter.attackMs, "finalLimiter.attackMs"),
    RADIO_FLOAT_ENTRY("releasems", radio.finalLimiter.releaseMs, "finalLimiter.releaseMs"),
}};

static const std::array<RadioSettingEntry, 1> kOutputSettings = {{
    RADIO_FLOAT_ENTRY("digitalmakeupgain", radio.output.digitalMakeupGain, "output.digitalMakeupGain"),
}};

static const std::array<RadioSettingEntry, 24> kNodesSettings = {{
    RADIO_BOOL_CUSTOM_ENTRY("tuning", "nodes.tuning", radio.graph.setEnabled(PassId::Tuning, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("input", "nodes.input", radio.graph.setEnabled(PassId::Input, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("avc", "nodes.avc", radio.graph.setEnabled(PassId::AVC, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("afc", "nodes.afc", radio.graph.setEnabled(PassId::AFC, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("controlbus", "nodes.controlbus", radio.graph.setEnabled(PassId::ControlBus, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("interferencederived", "nodes.interferencederived", radio.graph.setEnabled(PassId::InterferenceDerived, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("frontend", "nodes.frontend", radio.graph.setEnabled(PassId::FrontEnd, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("mixer", "nodes.mixer", radio.graph.setEnabled(PassId::Mixer, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("ifstrip", "nodes.ifstrip", radio.graph.setEnabled(PassId::IFStrip, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("demod", "nodes.demod", radio.graph.setEnabled(PassId::Demod, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("detectoraudio", "nodes.detectoraudio", radio.graph.setEnabled(PassId::DetectorAudio, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("audiodetector", "nodes.detectoraudio", radio.graph.setEnabled(PassId::DetectorAudio, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("receiverinputnetwork", "nodes.receiverinputnetwork", radio.graph.setEnabled(PassId::ReceiverInputNetwork, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("detectorload", "nodes.receiverinputnetwork", radio.graph.setEnabled(PassId::ReceiverInputNetwork, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("receiverinput", "nodes.receiverinputnetwork", radio.graph.setEnabled(PassId::ReceiverInputNetwork, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("receivercircuit", "nodes.receivercircuit", radio.graph.setEnabled(PassId::ReceiverCircuit, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("tone", "nodes.tone", radio.graph.setEnabled(PassId::Tone, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("power", "nodes.power", radio.graph.setEnabled(PassId::Power, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("noise", "nodes.noise", radio.graph.setEnabled(PassId::Noise, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("speaker", "nodes.speaker", radio.graph.setEnabled(PassId::Speaker, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("cabinet", "nodes.cabinet", radio.graph.setEnabled(PassId::Cabinet, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("outputscale", "nodes.outputscale", radio.graph.setEnabled(PassId::OutputScale, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("finallimiter", "nodes.finallimiter", radio.graph.setEnabled(PassId::FinalLimiter, parsed)),
    RADIO_BOOL_CUSTOM_ENTRY("outputclip", "nodes.outputclip", radio.graph.setEnabled(PassId::OutputClip, parsed)),
}};

static const std::array<RadioSettingSection, 15> kRadioSettingSections = {{
    makeRadioSettingSection("globals", kGlobalsSettings),
    makeRadioSettingSection("tuning", kTuningSettings),
    makeRadioSettingSection("frontend", kFrontEndSettings),
    makeRadioSettingSection("mixer", kMixerSettings),
    makeRadioSettingSection("ifstrip", kIfStripSettings),
    makeRadioSettingSection("demod", kDemodSettings),
    makeRadioSettingSection("receivercircuit", kReceiverCircuitSettings),
    makeRadioSettingSection("tone", kToneSettings),
    makeRadioSettingSection("power", kPowerSettings),
    makeRadioSettingSection("noise", kNoiseSettings),
    makeRadioSettingSection("speaker", kSpeakerSettings),
    makeRadioSettingSection("cabinet", kCabinetSettings),
    makeRadioSettingSection("finallimiter", kFinalLimiterSettings),
    makeRadioSettingSection("output", kOutputSettings),
    makeRadioSettingSection("nodes", kNodesSettings, "node", false),
}};


#undef RADIO_FLOAT_ENTRY
#undef RADIO_INT_ENTRY
#undef RADIO_BOOL_ENTRY
#undef RADIO_BOOL_CUSTOM_ENTRY

static const RadioSettingSection* findRadioSettingSection(
    const std::string& sectionNorm) {
  for (const auto& section : kRadioSettingSections) {
    if (sectionNorm == section.name) {
      return &section;
    }
  }
  return nullptr;
}

static const RadioSettingEntry* findRadioSettingEntry(
    const RadioSettingSection& section,
    const std::string& keyNorm) {
  for (size_t i = 0; i < section.settingCount; ++i) {
    if (keyNorm == section.settings[i].key) {
      return &section.settings[i];
    }
  }
  return nullptr;
}

static bool applyRadioSettingsValue(Radio1938& radio,
                                   const std::string& sectionNorm,
                                   const std::string& keyNorm,
                                   const std::string& valueText,
                                   int lineNumber,
                                   std::string* error) {
  const RadioSettingSection* section = findRadioSettingSection(sectionNorm);
  if (!section) {
    if (error) {
      *error = iniError(lineNumber, "unknown setting '" + sectionNorm + "." +
                                        keyNorm + "' in settings file");
    }
    return false;
  }

  const RadioSettingEntry* entry = findRadioSettingEntry(*section, keyNorm);
  if (!entry) {
    if (error) {
      std::string keyPath = keyNorm;
      if (section->unknownIncludesSection) {
        keyPath = std::string(section->name) + "." + keyNorm;
      }
      *error = iniError(lineNumber, std::string("unknown ") +
                                        section->unknownLabel + " '" + keyPath +
                                        "' in settings file");
    }
    return false;
  }

  return entry->apply(radio, valueText, lineNumber, error);
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
