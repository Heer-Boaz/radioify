#ifndef APP_COMMON_H
#define APP_COMMON_H

#include <string>

struct Options {
  std::string input;
  std::string output;
  bool extractSheet = false;
  int bwHz = 5500;
  double noise = 0.012;
  bool mono = true;
  bool play = true;
  bool dry = false;
  bool enableAscii = true;
  bool enableAudio = true;
  bool enableRadio = false;
  bool enableWindow = false;
  bool verbose = false;
};

void die(const std::string& message);
void logLine(const std::string& message);
Options parseArgs(int argc, char** argv);

#endif  // APP_COMMON_H
