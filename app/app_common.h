#ifndef APP_COMMON_H
#define APP_COMMON_H

#include <string>

#include "audio/audiofilter/radio1938/radio_reception_profile.h"
#include "core/runtime_defaults.h"
#include "core/shell_open_mode.h"

struct Options {
  std::string input;
  std::string output;
  std::string radioSettingsPath;
  std::string radioPresetName;
  RadioReceptionProfile radioReceptionProfile =
      kDefaultRadioReceptionProfile;
  int trackIndex = 0;
  bool splitLoop = false;
  bool extractSheet = false;
  bool renderRadio = false;
  bool force50Hz = false;
  int bwHz = kDefaultRadioBandwidthHz;
  double noise = kDefaultRadioNoiseAmount;
  bool calibrationReport = false;
  bool measureNodeSteps = false;
  bool clickTraceReport = false;
  double clickTraceThreshold = 0.05;
  int clickTraceEvents = 12;
  bool mono = kDefaultMonoAudioEnabled;
  bool play = true;
  bool dry = kDefaultDryAudioEnabled;
  bool enableAscii = kDefaultAsciiPlaybackEnabled;
  bool enableAudio = kDefaultAudioPlaybackEnabled;
  bool enableRadio = kDefaultRadioFilterEnabled;
  bool enableWindow = kDefaultWindowPlaybackEnabled;
  bool asciiDebugOverlay = false;
  bool shellOpen = false;
  ShellOpenModeSelection shellOpenMode = ShellOpenModeSelection::Configured;
  bool verbose = false;
};

void die(const std::string& message);
void logLine(const std::string& message);
Options parseArgs(int argc, char** argv);

#endif  // APP_COMMON_H
