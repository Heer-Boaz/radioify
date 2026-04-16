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

struct RadioifyTerminalFontMetrics {
  UINT dpi = USER_DEFAULT_SCREEN_DPI;
  int fontPixelHeight = 16;
  int fontWeight = FW_NORMAL;
  int cellWidth = 9;
  int cellHeight = 21;
  int ascent = 17;
  int descent = 4;
  int internalLeading = 5;
  int externalLeading = 0;
};

inline const wchar_t* radioifyTerminalFontFace() {
  return L"Cascadia Mono";
}

inline int radioifyTerminalFontPointSize() {
  return 12;
}

inline int radioifyTerminalFontWeight() {
  return FW_NORMAL;
}

inline int radioifyTerminalFontPixelHeightForDpi(UINT dpi) {
  return MulDiv(radioifyTerminalFontPointSize(), std::max<UINT>(1, dpi), 72);
}

inline HFONT createRadioifyTerminalFontForDpi(
    UINT dpi,
    int weight = radioifyTerminalFontWeight(),
    DWORD quality = ANTIALIASED_QUALITY) {
  return CreateFontW(-radioifyTerminalFontPixelHeightForDpi(dpi), 0, 0, 0,
                     weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, quality,
                     FIXED_PITCH | FF_DONTCARE, radioifyTerminalFontFace());
}

inline HFONT createRadioifyTerminalFont(int pixelHeight,
                                        int weight = radioifyTerminalFontWeight(),
                                        DWORD quality = ANTIALIASED_QUALITY) {
  return CreateFontW(-std::max(1, pixelHeight), 0, 0, 0, weight, FALSE, FALSE,
                     FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, quality, FIXED_PITCH | FF_DONTCARE,
                     radioifyTerminalFontFace());
}

inline RadioifyTerminalFontMetrics measureRadioifyTerminalFontMetrics(
    HDC hdc,
    UINT dpi = USER_DEFAULT_SCREEN_DPI,
    int requestedWeight = radioifyTerminalFontWeight()) {
  RadioifyTerminalFontMetrics metrics{};
  metrics.dpi = std::max<UINT>(1, dpi);
  metrics.fontPixelHeight =
      radioifyTerminalFontPixelHeightForDpi(metrics.dpi);
  metrics.fontWeight = std::clamp(requestedWeight, 1, 1000);

  HFONT font = createRadioifyTerminalFontForDpi(metrics.dpi, metrics.fontWeight);
  if (!font) {
    return metrics;
  }

  HGDIOBJ oldFont = SelectObject(hdc, font);
  TEXTMETRICW tm{};
  SIZE extent{};
  if (GetTextMetricsW(hdc, &tm)) {
    metrics.fontWeight =
        std::clamp(static_cast<int>(tm.tmWeight), 1, 1000);
    metrics.ascent = std::max(1, static_cast<int>(tm.tmAscent));
    metrics.descent = std::max(0, static_cast<int>(tm.tmDescent));
    metrics.internalLeading =
        std::max(0, static_cast<int>(tm.tmInternalLeading));
    metrics.externalLeading =
        std::max(0, static_cast<int>(tm.tmExternalLeading));
    metrics.cellHeight = std::max(
        1, static_cast<int>(tm.tmHeight + tm.tmExternalLeading));
    metrics.cellWidth = std::max(1, static_cast<int>(tm.tmAveCharWidth));
  }
  if (GetTextExtentPoint32W(hdc, L"M", 1, &extent)) {
    metrics.cellWidth = std::max(1, static_cast<int>(extent.cx));
  }

  if (oldFont) {
    SelectObject(hdc, oldFont);
  }
  DeleteObject(font);
  return metrics;
}

inline HFONT createRadioifyTerminalFont(
    const RadioifyTerminalFontMetrics& metrics,
    DWORD quality = ANTIALIASED_QUALITY) {
  return createRadioifyTerminalFontForDpi(
      metrics.dpi, std::clamp(metrics.fontWeight, 1, 1000), quality);
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
