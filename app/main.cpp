#include "app_common.h"
#include "crash_handler.h"
#include "core/windows_app_identity.h"
#include "tui/tui.h"

int main(int argc, char** argv) {
  initializeWindowsAppIdentity();
  installCrashHandler();
  Options o = parseArgs(argc, argv);
  return runTui(o);
}
