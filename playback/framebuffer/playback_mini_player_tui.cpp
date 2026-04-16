#include "playback_mini_player_tui.h"

#include <algorithm>

namespace playback_framebuffer_presenter {
namespace {

uint32_t color(GpuTextGridColor value) {
  return gpuTextGridColorRgb(value);
}

GpuTextGridCell makeCell(char ch, GpuTextGridColor fg, GpuTextGridColor bg) {
  return GpuTextGridCell{static_cast<uint32_t>(
                             static_cast<unsigned char>(ch)),
                         color(fg), color(bg), 0};
}

uint32_t color(const Color& value) {
  return gpuTextGridRgb(value.r, value.g, value.b);
}

bool shaderGlyphSupports(char ch) {
  if (ch >= 'A' && ch <= 'Z') return true;
  if (ch >= '0' && ch <= '9') return true;
  switch (ch) {
    case ' ':
    case '#':
    case '%':
    case '(':
    case ')':
    case '+':
    case '-':
    case '.':
    case '/':
    case ':':
    case '?':
    case '[':
    case '\\':
    case ']':
    case '_':
    case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

GpuTextGridCell makeScreenCell(const ScreenCell& cell) {
  const uint32_t fg = color(cell.fg);
  const uint32_t bg = color(cell.bg);
  if (cell.continuation) {
    return GpuTextGridCell{' ', fg, bg, 0};
  }

  wchar_t ch = cell.ch;
  if (ch >= 0x2800 && ch <= 0x28FF) {
    return GpuTextGridCell{static_cast<uint32_t>(ch - 0x2800), fg, bg,
                           kGpuTextGridCellFlagBraille};
  }
  if (ch >= 0x2588 && ch <= 0x258F) {
    return GpuTextGridCell{' ', fg, fg, 0};
  }

  char out = '?';
  if (ch >= 32 && ch <= 126) {
    out = static_cast<char>(ch);
    if (out >= 'a' && out <= 'z') out = static_cast<char>(out - 'a' + 'A');
    if (out == '\\') out = '/';
    if (!shaderGlyphSupports(out)) out = '?';
  } else {
    switch (ch) {
      case L'\u25B6':
      case L'\u23F8':
        out = 'P';
        break;
      case L'\u25A0':
      case L'\u2022':
        out = '#';
        break;
      case L'\u2013':
      case L'\u2014':
      case L'\u2212':
        out = '-';
        break;
      case L'\u2502':
      case L'\u2503':
        out = '|';
        break;
      case L'\u2500':
      case L'\u2501':
        out = '-';
        break;
      case L'\u2514':
      case L'\u2518':
      case L'\u250C':
      case L'\u2510':
      case L'\u253C':
        out = '+';
        break;
      default:
        out = ' ';
        break;
    }
  }
  return GpuTextGridCell{static_cast<uint32_t>(
                             static_cast<unsigned char>(out)),
                         fg, bg, 0};
}

}  // namespace

void buildGpuTextGridFrameFromScreenCells(const std::vector<ScreenCell>& cells,
                                          int cols, int rows,
                                          GpuTextGridFrame& outFrame) {
  cols = std::max(1, cols);
  rows = std::max(1, rows);
  outFrame.cols = cols;
  outFrame.rows = rows;

  const GpuTextGridCell background =
      makeCell(' ', GpuTextGridColor::Text, GpuTextGridColor::Background);
  const size_t cellCount =
      static_cast<size_t>(cols) * static_cast<size_t>(rows);
  if (outFrame.cells.size() != cellCount) {
    outFrame.cells.assign(cellCount, background);
  }

  const size_t available = std::min(cellCount, cells.size());
  for (size_t i = 0; i < available; ++i) {
    outFrame.cells[i] = makeScreenCell(cells[i]);
  }
  for (size_t i = available; i < cellCount; ++i) {
    outFrame.cells[i] = background;
  }
}

}  // namespace playback_framebuffer_presenter
