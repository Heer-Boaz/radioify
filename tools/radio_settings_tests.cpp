#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

#include "audiofilter/config/radio_settings_document.h"
#include "audiofilter/config/radio_settings_schema.h"
#include "radio.h"

namespace {
void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "radio_settings_tests: " << message << '\n';
    std::exit(1);
  }
}

class SettingsFile {
 public:
  SettingsFile()
      : path_(
            std::filesystem::temp_directory_path() /
            ("radioify-settings-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             ".toml")) {}
  ~SettingsFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  void write(const std::string& text) {
    std::ofstream file(path_);
    file << text;
  }
  bool apply(Radio1938& radio, const std::string& preset = "test") {
    error.clear();
    return applyRadioSettingsIni(radio, path_.string(), preset, &error);
  }
  std::string error;

 private:
  std::filesystem::path path_;
};

void testSchemaCoverage() {
  using namespace radio_settings;
  Radio1938 original, restored;
  original.applyReceiverProfile(RadioReceiverProfile::Philco37116);
  restored.applyReceiverProfile(RadioReceiverProfile::Philco37116);
  std::set<std::string> sections;
  std::ostringstream file;
  file << std::setprecision(9);
  const auto registry = schema();
  for (size_t i = 0; i < registry.count; ++i) {
    const auto& section = registry.sections[i];
    check(sections.insert(normalizedName(section.name)).second,
          "duplicate section registration");
    check(findSection(section.name) == &section,
          "section cannot resolve its own registration");
    std::set<std::string> names;
    file << "[audio-filter.test." << section.name << "]\n";
    for (size_t j = 0; j < section.count; ++j) {
      const auto& field = section.fields[j];
      check(names.insert(normalizedName(field.name)).second,
            "ambiguous field registration");
      check(findField(section, field.name) == &field,
            "field cannot resolve its own registration");
      file << field.name << " = ";
      std::visit([&](auto value) { file << std::boolalpha << value; },
                 read(field, original));
      file << '\n';
    }
  }
  SettingsFile settings;
  settings.write(file.str());
  check(settings.apply(restored), "complete schema could not be loaded from settings");
  for (size_t i = 0; i < registry.count; ++i) {
    const auto& section = registry.sections[i];
    for (size_t j = 0; j < section.count; ++j)
      check(read(section.fields[j], original) == read(section.fields[j], restored),
            "setting lost precision or type");
  }
}

void testDocumentBoundary() {
  using namespace radio_settings;
  Document document;
  std::string error;
  // The parser accepts application-independent section names. Schema lookup
  // owns their meaning; adding a section never changes this parser.
  std::istringstream stream(
      "\xEF\xBB\xBF[audio-filter.test.future-section]\n"
      "value = '3#4;5' # outside comment\n");
  check(parseDocument(stream, document, &error),
        "structural parser rejected an unknown section");
  check(document.presets.at("test")[0].text == "3#4;5",
        "comment stripped inside a quoted value");
  std::istringstream malformed("[audio-filter.test.room\nenabled = true\n");
  check(!parseDocument(malformed, document, &error) &&
            error.find("line 1") != std::string::npos,
        "malformed header lost its source location");
  check(document.presets.at("test")[0].text == "3#4;5",
        "failed parse modified the previous document");
}

void testPresetAndAliasCompatibility() {
  SettingsFile settings;
  Radio1938 radio;
  settings.write(
      "[audio-filter.default.globals]\ninputPad = .75\n"
      "[audio-filter.other.globals]\ninputPad = .25\n"
      "[audio-filter.default.nodes]\naudio-detector = OFF\nreceiver_input = no\n"
      "[audio-filter.default.room]\nHeadphones = false\nreflections = .2\n");
  check(settings.apply(radio, ""), "default preset was not selected");
  check(radio.globals.inputPad == .75f && !radio.graph.isEnabled(PassId::DetectorAudio) &&
            !radio.graph.isEnabled(PassId::ReceiverInputNetwork),
        "legacy names or boolean aliases changed");
  check(!radio.room.headphones && radio.room.reflections == .2f,
        "room settings did not reach renderer config");
  check(settings.apply(radio, "OTHER") && radio.globals.inputPad == .25f,
        "explicit preset selection changed");
  check(!settings.apply(radio, "missing"), "unknown preset accepted");
  settings.write("[audio-filter.only.output]\ndigitalMakeupGain = .4\n");
  check(settings.apply(radio, "") && radio.output.digitalMakeupGain == .4f,
        "single-preset selection changed");
}

void testFailuresLeavePlaybackUntouched() {
  SettingsFile settings;
  Radio1938 radio;
  radio.room.enabled = false;
  radio.init(1, 48000, 5500, .012f);
  radio.warmInputCarrier(.12f, 1024);
  const auto initial = radio;
  constexpr const char* invalidFiles[]{
      "[audio-filter.test.globals]\ninputPad = .25\nunknown = 2\n",
      "[audio-filter.test.nodes]\npower = false\n[ audio-filter.test.room ]\nwidthMeters "
      "= 1\n",
      "[audio-filter.test.globals]\ninputPad = nan\n",
      "[audio-filter.test.globals]\ninputPad = inf\n",
      "[audio-filter.test.globals]\ninputPad = 1e100\n",
      "[audio-filter.test.globals]\noversampleFactor = 0\n",
      "[audio-filter.test.globals]\noversampleFactor = 1.5\n",
      "[audio-filter.test.globals]\noversampleFactor = 3\n",
      "[audio-filter.test.globals]\noversampleFactor = 16\n",
      "[audio-filter.test.globals]\noversampleCutoffFraction = 0\n",
      "[audio-filter.test.globals]\noversampleCutoffFraction = .5\n",
      "[audio-filter.test.power]\noutputTransformerIntegrationSubsteps = "
      "99999999999999999999\n",
      "[audio-filter.test.room]\nenabled = tru!!e\n",
      "[audio-filter.test.speaker]\nmotorTravelMeters = -1\n",
      "[audio-filter.test.receivercircuit]\nvolumeControlResistanceOhms = 0\n",
      "[audio-filter.test.receivercircuit]\ngridLeakResistanceOhms = -1\n",
      "[audio-filter.test.receivercircuit]\nvolumeControlPosition = 1.1\n",
      "[audio-filter.test.receivercircuit]\nvolumeControlTapResistanceOhms = 1e9\n",
      "[audio-filter.test.receivercircuit]\ncouplingCapFarads = -1e-9\n",
      "[audio-filter.test.receivercircuit]\ntubeDcFeedResistanceOhms = -1\n",
      "[audio-filter.test.receivercircuit]\ntubeLoadResistanceOhms = 0\n",
      "[audio-filter.test.receivercircuit]\ntubePlateCurrentAmps = .009\n",
      "[audio-filter.test.ifstrip]\ntransformerCount = 0\n",
      "[audio-filter.test.ifstrip]\ntransformerCount = 4\n",
      "[audio-filter.test.ifstrip]\nprimaryInductanceHenries = 0\n",
      "[audio-filter.test.ifstrip]\nsecondarySeriesResistanceOhms = -1\n",
      "[audio-filter.test.ifstrip]\nreferenceTransimpedanceOhms = 0\n",
      "[audio-filter.test.ifstrip]\noutputCouplingCoeff = -.1\n",
      "[audio-filter.test.ifstrip]\ninterstageCouplingCoeff = .5\n",
      "[audio-filter.test.tuning]\nafcSignalReferenceVolts = 0\n",
      "[audio-filter.test.tuning]\nafcLoopGain = 0\n",
      "[audio-filter.test.tuning]\nsmoothTau = 0\n",
      "[audio-filter.test.noise]\nhissMaskDepth = .2\n",
      "[audio-filter.test.demod]\naudioDiodeThermalVolts = 0\n",
      "[audio-filter.test.demod]\naudioDiodePerveance = -1\n",
      "[audio-filter.test.powersupply]\nreservoirFarads = 0\n",
      "[audio-filter.test.powersupply]\nfilterHenries = -1\n",
      "[audio-filter.test.power]\nrippleDepth = .02\n",
      "[audio-filter.test.demod]\naudioJunctionSlopeVolts = .0045\n",
      "[audio-filter.test.unknown]\nvalue = 1\n",
  };
  for (const auto* contents : invalidFiles) {
    radio = initial;
    auto expected = initial;
    settings.write(contents);
    check(!settings.apply(radio) && !settings.error.empty(),
          "invalid settings accepted without an error");
    check(radio.globals.inputPad == initial.globals.inputPad &&
              radio.graph.isEnabled(PassId::Power) &&
              radio.room.widthMeters == initial.room.widthMeters,
          "failed load left a partial configuration");
    float program[64]{}, actual[64]{}, reference[64]{};
    for (size_t i = 0; i < 64; ++i) program[i] = i & 1 ? .2f : -.2f;
    radio.processAmAudio(program, actual, 64, .12f, .85f);
    expected.processAmAudio(program, reference, 64, .12f, .85f);
    for (size_t i = 0; i < 64; ++i)
      check(actual[i] == reference[i], "failed load changed live DSP history");
  }
  settings.write(
      "[audio-filter.test.speaker]\nmotorTravelMeters = .002\n"
      "[audio-filter.test.room]\nreflections = .15\n");
  check(settings.apply(radio), "valid initialized update failed");
  check(radio.speakerStage.speaker.motorTravelMeters == .002f &&
            radio.speakerStage.speaker.coneDisplacementMeters == 0 &&
            radio.room.reflections == .15f,
        "successful update did not rebuild the receiver with its complete configuration");
  for (int factor : {1, 2, 4, 8}) {
    settings.write("[audio-filter.test.globals]\noversampleFactor = " +
                   std::to_string(factor) + "\n");
    check(settings.apply(radio), "valid oversampling factor rejected");
    check(radio.power.internalSampleRate == radio.sampleRate * factor &&
              radio.receiverCircuit.oversampler.factor() == factor &&
              radio.speakerStage.oversampler.factor() == factor &&
              radio.output.clipOversampler.factor() == factor &&
              radio.finalLimiter.peakOversampler.factor() == factor,
          "configuration did not update every nonlinear island's clock");
  }
}
}  // namespace

int main() {
  testSchemaCoverage();
  testDocumentBoundary();
  testPresetAndAliasCompatibility();
  testFailuresLeavePlaybackUntouched();
  std::cout << "Settings schema, compatibility and transaction checks passed\n";
}
