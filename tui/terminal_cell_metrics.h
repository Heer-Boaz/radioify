#ifndef TERMINAL_CELL_METRICS_H
#define TERMINAL_CELL_METRICS_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

enum class TerminalCellMetricSource {
  None,
  Csi16CellSize,
  Win32ConsoleFont,
  Csi14TextArea,
  BrailleGlyphInk,
};

struct TerminalCellPixelSize {
  double width = 0.0;
  double height = 0.0;
  TerminalCellMetricSource source = TerminalCellMetricSource::None;
};

const char* terminalCellMetricSourceLabel(TerminalCellMetricSource source);
TerminalCellPixelSize queryTerminalDirectCellPixelSize(HANDLE input,
                                                       HANDLE output,
                                                       DWORD timeoutMs);
TerminalCellPixelSize queryTerminalTextAreaCellPixelSize(HANDLE input,
                                                         HANDLE output,
                                                         int columns,
                                                         int rows,
                                                         DWORD timeoutMs);
TerminalCellPixelSize queryTerminalCellPixelSize(HANDLE input, HANDLE output,
                                                 int columns, int rows,
                                                 DWORD timeoutMs);
TerminalCellPixelSize queryConsoleFontCellPixelSize(HANDLE output);
TerminalCellPixelSize queryBrailleGlyphCellPixelSize(HANDLE output,
                                                     double cellPixelWidth,
                                                     double cellPixelHeight);

#endif
