#include "app_common.h"

#include <cstdlib>
#include <iostream>

static void showUsage(const char* exe) {
  std::string name = exe ? std::string(exe) : "radioify";
  logLine("Usage: " + name + " [options] [file_or_folder]");
  logLine("Options:");
  logLine("  split-loop <file>      Analyze and split audio into stinger + main loop");
  logLine("  extract-sheet <file>   Analyze a track and write .melody + .mid output");
  logLine("  --extract-sheet <file> Same as extract-sheet");
  logLine("  --split-loop <file>    Same as split-loop");
  logLine("  out <path>             Generic output path (for extract/render flows)");
  logLine("  --out <path>           Same as out");
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
      if (o.splitLoop) {
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
      if (o.extractSheet) {
        die("Only one analysis command can be used at once.");
      }
      if (!o.input.empty() && o.input != value) {
        die("Input path was provided multiple times.");
      }
      o.splitLoop = true;
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
    if (!arg.empty() && arg[0] == '-') {
      die("Unknown option: " + arg);
    }
    if (o.extractSheet || o.splitLoop) {
      die("Do not pass a positional input when using extract-sheet/split-loop.");
    }
    if (!o.input.empty()) {
      die("Provide a single file or folder path only.");
    }
    o.input = arg;
  }
  return o;
}
