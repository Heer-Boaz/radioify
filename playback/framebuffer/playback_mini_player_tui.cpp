#include "playback_mini_player_tui.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

#include "ui_helpers.h"

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

GpuTextGridCell makeAsciiCell(const AsciiArt::AsciiCell& cell) {
  const uint32_t bg = cell.hasBg ? color(cell.bg)
                                 : color(GpuTextGridColor::Background);
  if (cell.ch >= 0x2800 && cell.ch <= 0x28FF) {
    return GpuTextGridCell{
        static_cast<uint32_t>(cell.ch - 0x2800), color(cell.fg), bg,
        kGpuTextGridCellFlagBraille};
  }
  if (cell.ch >= 32 && cell.ch <= 126) {
    char ch = static_cast<char>(cell.ch);
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    return GpuTextGridCell{static_cast<uint32_t>(
                               static_cast<unsigned char>(ch)),
                           color(cell.fg), bg, 0};
  }
  return GpuTextGridCell{'?', color(cell.fg), bg, 0};
}

size_t utf8Advance(std::string_view text, size_t offset) {
  unsigned char ch = static_cast<unsigned char>(text[offset]);
  if (ch < 0x80) return offset + 1;
  if ((ch & 0xE0u) == 0xC0u) return std::min(offset + 2, text.size());
  if ((ch & 0xF0u) == 0xE0u) return std::min(offset + 3, text.size());
  if ((ch & 0xF8u) == 0xF0u) return std::min(offset + 4, text.size());
  return offset + 1;
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

std::string normalizeText(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    unsigned char raw = static_cast<unsigned char>(text[i]);
    if (raw < 0x80) {
      char ch = static_cast<char>(raw);
      if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
      if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
      if (ch == '\\') ch = '/';
      out.push_back(shaderGlyphSupports(ch) ? ch : '?');
      ++i;
      continue;
    }
    out.push_back('?');
    i = utf8Advance(text, i);
  }
  return out;
}

void setCell(GpuTextGridFrame& frame, int x, int y, char ch,
             GpuTextGridColor fg, GpuTextGridColor bg) {
  if (x < 0 || y < 0 || x >= frame.cols || y >= frame.rows) return;
  frame.cells[static_cast<size_t>(y) * static_cast<size_t>(frame.cols) +
              static_cast<size_t>(x)] = makeCell(ch, fg, bg);
}

void fillRect(GpuTextGridFrame& frame, int x0, int y0, int x1, int y1,
              GpuTextGridColor bg) {
  x0 = std::clamp(x0, 0, frame.cols);
  x1 = std::clamp(x1, 0, frame.cols);
  y0 = std::clamp(y0, 0, frame.rows);
  y1 = std::clamp(y1, 0, frame.rows);
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      setCell(frame, x, y, ' ', GpuTextGridColor::Text, bg);
    }
  }
}

void drawText(GpuTextGridFrame& frame, int x, int y, const std::string& text,
              GpuTextGridColor fg, GpuTextGridColor bg) {
  int cursor = x;
  for (char ch : text) {
    if (cursor >= frame.cols) return;
    setCell(frame, cursor++, y, ch, fg, bg);
  }
}

void drawCentered(GpuTextGridFrame& frame, int y, const std::string& text,
                  GpuTextGridColor fg, GpuTextGridColor bg) {
  int x = std::max(1, (frame.cols - static_cast<int>(text.size())) / 2);
  drawText(frame, x, y, text, fg, bg);
}

void drawBorder(GpuTextGridFrame& frame) {
  for (int x = 0; x < frame.cols; ++x) {
    setCell(frame, x, 0, '-', GpuTextGridColor::Dim,
            GpuTextGridColor::Background);
    setCell(frame, x, frame.rows - 1, '-', GpuTextGridColor::Dim,
            GpuTextGridColor::Background);
  }
  for (int y = 0; y < frame.rows; ++y) {
    setCell(frame, 0, y, '|', GpuTextGridColor::Dim,
            GpuTextGridColor::Background);
    setCell(frame, frame.cols - 1, y, '|', GpuTextGridColor::Dim,
            GpuTextGridColor::Background);
  }
  setCell(frame, 0, 0, '#', GpuTextGridColor::Dim,
          GpuTextGridColor::Background);
  setCell(frame, frame.cols - 1, 0, '#', GpuTextGridColor::Dim,
          GpuTextGridColor::Background);
  setCell(frame, 0, frame.rows - 1, '#', GpuTextGridColor::Dim,
          GpuTextGridColor::Background);
  setCell(frame, frame.cols - 1, frame.rows - 1, '#', GpuTextGridColor::Dim,
          GpuTextGridColor::Background);
}

double progressRatio(const WindowUiState& ui) {
  if (ui.totalSec > 0.0 && std::isfinite(ui.totalSec)) {
    return std::clamp(ui.displaySec / ui.totalSec, 0.0, 1.0);
  }
  return std::clamp(static_cast<double>(ui.progress), 0.0, 1.0);
}

std::string buildStatusLine(const WindowUiState& ui) {
  std::string status = ui.isPaused ? "PAUSE " : "PLAY ";
  std::string time = formatTime(ui.displaySec);
  if (ui.totalSec > 0.0 && std::isfinite(ui.totalSec)) {
    time += " / " + formatTime(ui.totalSec);
  }
  return normalizeText(status + time + " V:" +
                       std::to_string(std::clamp(ui.volPct, 0, 100)) + "%");
}

void drawProgress(GpuTextGridFrame& frame, int y, double ratio) {
  const int x0 = 2;
  const int width = std::max(1, frame.cols - 4);
  const int fill =
      std::clamp(static_cast<int>(std::lround(ratio * width)), 0, width);
  for (int x = 0; x < width; ++x) {
    setCell(frame, x0 + x, y, ' ', GpuTextGridColor::Text,
            x < fill ? GpuTextGridColor::Fill : GpuTextGridColor::Empty);
  }
}

void drawControls(GpuTextGridFrame& frame, const WindowUiState& ui, int y) {
  int x = 2;
  const int limit = frame.cols - 2;
  if (!ui.controlButtons.empty()) {
    for (size_t i = 0; i < ui.controlButtons.size() && x < limit; ++i) {
      if (i > 0) {
        setCell(frame, x++, y, ' ', GpuTextGridColor::Dim,
                GpuTextGridColor::Panel);
        if (x < limit) {
          setCell(frame, x++, y, ' ', GpuTextGridColor::Dim,
                  GpuTextGridColor::Panel);
        }
      }
      std::string label = normalizeText(ui.controlButtons[i].text);
      label = fitLine(label, std::max(0, limit - x));
      drawText(frame, x, y, label,
               ui.controlButtons[i].active ? GpuTextGridColor::Accent
                                           : GpuTextGridColor::Dim,
               GpuTextGridColor::Panel);
      x += static_cast<int>(label.size());
    }
    return;
  }

  std::string controls = fitLine(normalizeText(ui.controls), limit - x);
  drawText(frame, x, y, controls, GpuTextGridColor::Dim,
           GpuTextGridColor::Panel);
}

}  // namespace

void buildPlaybackMiniPlayerTuiFrame(const WindowUiState& ui, int pixelWidth,
                                     int pixelHeight,
                                     GpuTextGridFrame& outFrame) {
  const int cols = std::clamp(pixelWidth / 10, 28, 72);
  const int rows = std::clamp(pixelHeight / 18, 8, 18);
  outFrame.cols = cols;
  outFrame.rows = rows;

  const GpuTextGridCell background =
      makeCell(' ', GpuTextGridColor::Text, GpuTextGridColor::Background);
  const size_t cellCount =
      static_cast<size_t>(cols) * static_cast<size_t>(rows);
  if (outFrame.cells.size() != cellCount) {
    outFrame.cells.assign(cellCount, background);
  } else {
    std::fill(outFrame.cells.begin(), outFrame.cells.end(), background);
  }

  drawBorder(outFrame);
  fillRect(outFrame, 1, 1, cols - 1, rows - 1, GpuTextGridColor::Panel);

  std::string title = normalizeText(ui.title.empty() ? "RADIOIFY" : ui.title);
  title = fitLine(title, std::max(1, cols - 4));
  drawCentered(outFrame, 1, title, GpuTextGridColor::Text,
               GpuTextGridColor::Panel);

  std::string status = fitLine(buildStatusLine(ui), std::max(1, cols - 4));
  drawCentered(outFrame, 2, status, GpuTextGridColor::Accent,
               GpuTextGridColor::Panel);

  const int progressY = std::clamp(rows / 2, 3, rows - 4);
  drawProgress(outFrame, progressY, progressRatio(ui));

  const int controlsY = rows - 3;
  drawControls(outFrame, ui, controlsY);

  if (!ui.subtitle.empty() && rows >= 10) {
    std::string subtitle = fitLine(normalizeText(ui.subtitle), cols - 4);
    drawCentered(outFrame, rows - 2, subtitle, GpuTextGridColor::Dim,
                 GpuTextGridColor::Panel);
  }
}

std::pair<int, int> computePlaybackMiniPlayerAsciiSize(int pixelWidth,
                                                       int pixelHeight,
                                                       int srcWidth,
                                                       int srcHeight) {
  const int maxCols = std::clamp(pixelWidth / 6, 24, 180);
  const int maxRows = std::clamp(pixelHeight / 12, 10, 96);
  AsciiArtLayout fitted =
      fitAsciiArtLayout(srcWidth, srcHeight, maxCols, maxRows);
  return {fitted.width, fitted.height};
}

void buildPlaybackMiniPlayerAsciiFrame(const AsciiArt& art,
                                       const WindowUiState& ui,
                                       GpuTextGridFrame& outFrame) {
  const int cols = std::max(1, art.width);
  const int rows = std::max(1, art.height);
  outFrame.cols = cols;
  outFrame.rows = rows;

  const GpuTextGridCell background =
      makeCell(' ', GpuTextGridColor::Text, GpuTextGridColor::Background);
  const size_t cellCount =
      static_cast<size_t>(cols) * static_cast<size_t>(rows);
  if (outFrame.cells.size() != cellCount) {
    outFrame.cells.assign(cellCount, background);
  } else {
    std::fill(outFrame.cells.begin(), outFrame.cells.end(), background);
  }

  const size_t artCellCount =
      static_cast<size_t>(std::max(0, art.width)) *
      static_cast<size_t>(std::max(0, art.height));
  if (art.width > 0 && art.height > 0 && art.cells.size() >= artCellCount) {
    for (int y = 0; y < rows; ++y) {
      for (int x = 0; x < cols; ++x) {
        outFrame.cells[static_cast<size_t>(y) *
                           static_cast<size_t>(cols) +
                       static_cast<size_t>(x)] =
            makeAsciiCell(art.cells[static_cast<size_t>(y) *
                                        static_cast<size_t>(art.width) +
                                    static_cast<size_t>(x)]);
      }
    }
  }

  const bool showChrome = (ui.overlayAlpha > 0.01f) || ui.isPaused;
  if (!showChrome || rows < 8 || cols < 24) {
    return;
  }

  const int panelRows = rows >= 14 ? 3 : 2;
  const int panelY = rows - panelRows;
  fillRect(outFrame, 0, panelY, cols, rows, GpuTextGridColor::Panel);
  drawProgress(outFrame, panelY, progressRatio(ui));
  if (panelRows >= 3) {
    std::string status = fitLine(buildStatusLine(ui), std::max(1, cols - 4));
    drawCentered(outFrame, panelY + 1, status, GpuTextGridColor::Accent,
                 GpuTextGridColor::Panel);
  }
  drawControls(outFrame, ui, rows - 1);
}

}  // namespace playback_framebuffer_presenter
