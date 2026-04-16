#pragma once

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

inline const wchar_t* radioifyTerminalFontFace() {
  return L"Cascadia Mono";
}

inline int radioifyTerminalFontPointSize() {
  return 12;
}

inline int radioifyTerminalFontPixelHeightForDpi(UINT dpi) {
  return MulDiv(radioifyTerminalFontPointSize(), std::max<UINT>(1, dpi), 72);
}

inline HFONT createRadioifyTerminalFont(int pixelHeight,
                                        int weight = FW_NORMAL,
                                        DWORD quality = ANTIALIASED_QUALITY) {
  return CreateFontW(-std::max(1, pixelHeight), 0, 0, 0, weight, FALSE, FALSE,
                     FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, quality, FIXED_PITCH | FF_DONTCARE,
                     radioifyTerminalFontFace());
}

inline int fitRadioifyTerminalFontHeightToCell(HDC hdc, int fontH, int cellW,
                                               int cellH) {
  int fitted = std::max(1, fontH);
  HFONT font = createRadioifyTerminalFont(fitted);
  if (!font) return fitted;
  HGDIOBJ oldFont = SelectObject(hdc, font);
  TEXTMETRICW tm{};
  SIZE extent{};
  if (GetTextMetricsW(hdc, &tm) &&
      GetTextExtentPoint32W(hdc, L"M", 1, &extent)) {
    const int maxTextH = std::max(1, cellH);
    const int maxTextW = std::max(1, cellW);
    double scaleH =
        tm.tmHeight > 0 ? static_cast<double>(maxTextH) / tm.tmHeight : 1.0;
    double scaleW =
        extent.cx > 0 ? static_cast<double>(maxTextW) / extent.cx : 1.0;
    double scale = std::min({1.0, scaleH, scaleW});
    if (scale < 0.98) {
      fitted = std::max(1, static_cast<int>(std::lround(fitted * scale)));
    }
  }
  if (oldFont) {
    SelectObject(hdc, oldFont);
  }
  DeleteObject(font);
  return fitted;
}

#endif
