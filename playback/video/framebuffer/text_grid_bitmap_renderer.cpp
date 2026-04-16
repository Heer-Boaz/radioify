#include "text_grid_bitmap_renderer.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "terminal_font.h"

bool renderScreenGridToBitmap(const ScreenCell* cells, int cols, int rows,
                              int width, int height,
                              std::vector<uint8_t>* outPixels) {
  if (!outPixels) {
    return false;
  }

  outPixels->clear();
  if (!cells || cols <= 0 || rows <= 0 || width <= 0 || height <= 0) {
    return false;
  }

  const size_t pixelBytes =
      static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
  if (pixelBytes == 0) {
    return false;
  }
  outPixels->resize(pixelBytes);

  for (int py = 0; py < height; ++py) {
    int cellY = static_cast<int>((static_cast<int64_t>(py) * rows) /
                                 std::max(1, height));
    cellY = std::clamp(cellY, 0, rows - 1);
    uint8_t* dstRow = outPixels->data() +
                      static_cast<size_t>(py) * static_cast<size_t>(width) * 4u;
    const size_t rowBase = static_cast<size_t>(cellY) * static_cast<size_t>(cols);
    for (int px = 0; px < width; ++px) {
      int cellX = static_cast<int>((static_cast<int64_t>(px) * cols) /
                                   std::max(1, width));
      cellX = std::clamp(cellX, 0, cols - 1);
      const auto& cell = cells[rowBase + static_cast<size_t>(cellX)];
      uint8_t* p = dstRow + static_cast<size_t>(px) * 4u;
      p[0] = cell.bg.b;
      p[1] = cell.bg.g;
      p[2] = cell.bg.r;
      p[3] = 255;
    }
  }

  HDC memDC = CreateCompatibleDC(nullptr);
  if (!memDC) {
    return true;
  }

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* dibBits = nullptr;
  HBITMAP dib =
      CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
  if (!dib || !dibBits) {
    if (dib) {
      DeleteObject(dib);
    }
    DeleteDC(memDC);
    return true;
  }

  HGDIOBJ oldBmp = SelectObject(memDC, dib);
  std::memcpy(dibBits, outPixels->data(), pixelBytes);
  SetBkMode(memDC, TRANSPARENT);

  const int cellH = std::max(1, height / rows);
  const int cellW = std::max(1, width / cols);
  const int baseFontH =
      std::max(8, static_cast<int>(std::round(cellH * 0.95f)));
  const int fontH =
      fitRadioifyTerminalFontHeightToCell(memDC, baseFontH, cellW, cellH);
  HFONT font = createRadioifyTerminalFont(fontH);
  const bool ownsFont = (font != nullptr);
  if (!font) {
    font = static_cast<HFONT>(GetStockObject(SYSTEM_FIXED_FONT));
  }
  HGDIOBJ oldFont = SelectObject(memDC, font);

  COLORREF lastFg = RGB(255, 255, 255);
  bool hasFg = false;
  for (int r = 0; r < rows; ++r) {
    int y0 = static_cast<int>((static_cast<int64_t>(r) * height) / rows);
    int y1 = static_cast<int>((static_cast<int64_t>(r + 1) * height) / rows);
    if (y1 <= y0) {
      y1 = y0 + 1;
    }
    for (int c = 0; c < cols; ++c) {
      const auto& cell =
          cells[static_cast<size_t>(r) * static_cast<size_t>(cols) +
                static_cast<size_t>(c)];
      if (cell.continuation) {
        continue;
      }
      const wchar_t ch = cell.ch ? cell.ch : L' ';
      if (ch == L' ') {
        continue;
      }

      int x0 = static_cast<int>((static_cast<int64_t>(c) * width) / cols);
      int span = std::max(1, static_cast<int>(cell.cellWidth));
      int x1 = static_cast<int>(
          (static_cast<int64_t>(std::min(cols, c + span)) * width) / cols);
      if (x1 <= x0) {
        x1 = x0 + 1;
      }

      const COLORREF fg = RGB(cell.fg.r, cell.fg.g, cell.fg.b);
      if (!hasFg || fg != lastFg) {
        SetTextColor(memDC, fg);
        lastFg = fg;
        hasFg = true;
      }

      RECT rc{x0, y0, x1, y1};
      DrawTextW(memDC, &ch, 1, &rc,
                DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    }
  }

  std::memcpy(outPixels->data(), dibBits, pixelBytes);

  if (oldFont) {
    SelectObject(memDC, oldFont);
  }
  if (ownsFont && font) {
    DeleteObject(font);
  }
  if (oldBmp) {
    SelectObject(memDC, oldBmp);
  }
  DeleteObject(dib);
  DeleteDC(memDC);
  return true;
}

#else

bool renderScreenGridToBitmap(const ScreenCell*, int, int, int, int,
                              std::vector<uint8_t>*) {
  return false;
}

#endif
