#include "app_common.h"

#include <cstdlib>
#include <iostream>

static void showUsage(const char* exe) {
  std::string name = exe ? std::string(exe) : "radioify";
  logLine("Usage: " + name + " [options] [file_or_folder]");
  logLine("Options:");
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
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      showUsage(argv[0]);
      std::exit(0);
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
    if (!o.input.empty()) {
      die("Provide a single file or folder path only.");
    }
    o.input = arg;
  }
  return o;
}
