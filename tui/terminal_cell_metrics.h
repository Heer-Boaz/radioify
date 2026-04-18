#ifndef TERMINAL_CELL_METRICS_H
#define TERMINAL_CELL_METRICS_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

struct TerminalCellPixelSize {
  double width = 0.0;
  double height = 0.0;
};

TerminalCellPixelSize queryTerminalCellPixelSize(HANDLE input, HANDLE output,
                                                 int columns, int rows,
                                                 DWORD timeoutMs);
TerminalCellPixelSize queryConsoleFontCellPixelSize(HANDLE output);

#endif
