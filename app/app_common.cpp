#include "app_common.h"

#include <cstdlib>
#include <iostream>
#include <limits>

static void showUsage(const char* exe) {
  std::string name = exe ? std::string(exe) : "radioify";
  logLine("Usage: " + name + " [options] [file_or_folder]");
  logLine("Options:");
  logLine("  render-radio <file>    Render the explicit radio filter chain to WAV");
  logLine("  split-loop <file>      Analyze and split audio into stinger + main loop");
  logLine("  extract-sheet <file>   Analyze a track and write .melody + .mid output");
  logLine("  --render-radio <file>  Same as render-radio");
  logLine("  --extract-sheet <file> Same as extract-sheet");
  logLine("  --split-loop <file>    Same as split-loop");
  logLine("  out <path>             Generic output path (for extract/render flows)");
  logLine("  --track <index>        Select track index for emulated formats");
  logLine("  --50hz                 Force 50Hz playback mode where supported");
  logLine("  --calibration-report   Dump per-stage radio metrics after render-radio");
  logLine("  --measure-node-steps   Render and report every disabled-node variant for render-radio");
  logLine("  --out <path>           Same as out");
  logLine("  --radio-settings <path> Override audio-filter settings from a .toml file");
  logLine("  --radio-preset <name>   Select named audio-filter preset from the settings file");
  logLine("  --dry        Bypass radio processing for render/playback");
  logLine("  --no-ascii   Disable ASCII video rendering");
  logLine("  --no-audio   Disable audio playback");
  logLine("  --no-radio   Start with radio filter disabled");
  logLine("  --window     Open a window for video playback");
  logLine("  -h, --help   Show this help");
}

void die(const std::string& message) {
  std::cerr << "ERROR: " << message << "\n";
  std::exit(1);
}

void logLine(const std::string& message) {
  std::cout << message << "\n";
}

Options parseArgs(int argc, char** argv) {
  Options o;
  auto requireValue = [&](const std::string& option, int* index) -> std::string {
    if (!index) return {};
    int i = *index;
    if (i + 1 >= argc) {
      die("Missing value for option: " + option);
    }
    ++(*index);
    return argv[*index];
  };

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      showUsage(argv[0]);
      std::exit(0);
    }
    if (arg == "extract-sheet" || arg == "--extract-sheet") {
      std::string value = requireValue(arg, &i);
      if (value.empty()) {
        die("extract-sheet requires a non-empty file path.");
      }
      if (o.splitLoop || o.renderRadio) {
        die("Only one analysis command can be used at once.");
      }
      if (!o.input.empty() && o.input != value) {
        die("Input path was provided multiple times.");
      }
      o.extractSheet = true;
      o.input = value;
      continue;
    }
    if (arg == "split-loop" || arg == "--split-loop") {
      std::string value = requireValue(arg, &i);
      if (value.empty()) {
        die("split-loop requires a non-empty file path.");
      }
      if (o.extractSheet || o.renderRadio) {
        die("Only one analysis command can be used at once.");
      }
      if (!o.input.empty() && o.input != value) {
        die("Input path was provided multiple times.");
      }
      o.splitLoop = true;
      o.input = value;
      continue;
    }
    if (arg == "render-radio" || arg == "--render-radio") {
      std::string value = requireValue(arg, &i);
      if (value.empty()) {
        die("render-radio requires a non-empty file path.");
      }
      if (o.extractSheet || o.splitLoop) {
        die("Only one analysis command can be used at once.");
      }
      if (!o.input.empty() && o.input != value) {
        die("Input path was provided multiple times.");
      }
      o.renderRadio = true;
      o.input = value;
      continue;
    }
    if (arg == "out" || arg == "--out") {
      std::string value = requireValue(arg, &i);
      if (value.empty()) {
        die("out requires a non-empty path.");
      }
      o.output = value;
      continue;
    }
    if (arg == "--radio-settings") {
      std::string value = requireValue(arg, &i);
      if (value.empty()) {
        die("--radio-settings requires a non-empty path.");
      }
      o.radioSettingsPath = value;
      continue;
    }
    if (arg == "--radio-preset") {
      std::string value = requireValue(arg, &i);
      if (value.empty()) {
        die("--radio-preset requires a non-empty preset name.");
      }
      o.radioPresetName = value;
      continue;
    }
    if (arg == "--track") {
      std::string value = requireValue(arg, &i);
      if (value.empty()) {
        die("--track requires a non-empty integer.");
      }
      char* end = nullptr;
      long parsed = std::strtol(value.c_str(), &end, 10);
      if (*end != '\0') {
        die("--track expects an integer.");
      }
      if (parsed < 0) {
        die("--track expects a non-negative integer.");
      }
      if (parsed > static_cast<long>(std::numeric_limits<int>::max())) {
        die("--track value is too large.");
      }
      o.trackIndex = static_cast<int>(parsed);
      continue;
    }
    if (arg == "--50hz") {
      o.force50Hz = true;
      continue;
    }
    if (arg == "--calibration-report") {
      o.calibrationReport = true;
      continue;
    }
    if (arg == "--measure-node-steps") {
      o.measureNodeSteps = true;
      continue;
    }
    if (arg == "--dry") {
      o.dry = true;
      continue;
    }
    if (arg == "--no-ascii") {
      o.enableAscii = false;
      continue;
    }
    if (arg == "--no-audio") {
      o.enableAudio = false;
      continue;
    }
    if (arg == "--no-radio") {
      o.enableRadio = false;
      continue;
    }
    if (arg == "--window") {
      o.enableWindow = true;
      continue;
    }
    if (arg == "--shell-open") {
      o.shellOpen = true;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      die("Unknown option: " + arg);
    }
    if (o.extractSheet || o.splitLoop || o.renderRadio) {
      die("Do not pass a positional input when using extract-sheet/split-loop/render-radio.");
    }
    if (!o.input.empty()) {
      die("Provide a single file or folder path only.");
    }
    o.input = arg;
  }
  return o;
}
