#include "app_common.h"
#include "crash_handler.h"
#include "tui/tui.h"

int main(int argc, char** argv) {
  installCrashHandler();
  Options o = parseArgs(argc, argv);
  return runTui(o);
}
