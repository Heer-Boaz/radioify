#include "app_common.h"
#include "tui.h"

int main(int argc, char** argv) {
  Options o = parseArgs(argc, argv);
  return runTui(o);
}
