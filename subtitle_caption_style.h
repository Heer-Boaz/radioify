#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <mutex>

struct CaptionStyleProfile {
  uint8_t textR = 255;
  uint8_t textG = 255;
  uint8_t textB = 255;
  float textAlpha = 1.0f;

  uint8_t backgroundR = 0;
  uint8_t backgroundG = 0;
  uint8_t backgroundB = 0;
  // Match VLC defaults: no background box unless user explicitly enables one.
  float backgroundAlpha = 0.0f;

  float sizeScale = 1.0f;
  // Windows "Proportional sans serif" maps best to VLC's default family on Windows.
  int fontStyle = 4;
  // Match VLC-style readability defaults when no explicit Windows effect is set.
  int fontEffect = 4;
};

inline float captionOpacityFromValue(DWORD raw, float defaultOpacity) {
  switch (raw) {
    case 1:
      return 1.0f;   // OneHundredPercent
    case 2:
      return 0.75f;  // SeventyFivePercent
    case 3:
      return 0.25f;  // TwentyFivePercent
    case 4:
      return 0.0f;   // ZeroPercent
    default:
      return std::clamp(defaultOpacity, 0.0f, 1.0f);
  }
}

inline void captionColorFromValue(DWORD raw, uint8_t defaultR, uint8_t defaultG,
                                  uint8_t defaultB, uint8_t* outR,
                                  uint8_t* outG, uint8_t* outB) {
  if (!outR || !outG || !outB) return;
  switch (raw) {
    case 1:
      *outR = 255;
      *outG = 255;
      *outB = 255;
      return;  // White
    case 2:
      *outR = 0;
      *outG = 0;
      *outB = 0;
      return;  // Black
    case 3:
      *outR = 255;
      *outG = 0;
      *outB = 0;
      return;  // Red
    case 4:
      *outR = 0;
      *outG = 255;
      *outB = 0;
      return;  // Green
    case 5:
      *outR = 0;
      *outG = 0;
      *outB = 255;
      return;  // Blue
    case 6:
      *outR = 255;
      *outG = 255;
      *outB = 0;
      return;  // Yellow
    case 7:
      *outR = 255;
      *outG = 0;
      *outB = 255;
      return;  // Magenta
    case 8:
      *outR = 0;
      *outG = 255;
      *outB = 255;
      return;  // Cyan
    default:
      *outR = defaultR;
      *outG = defaultG;
      *outB = defaultB;
      return;  // Default
  }
}

inline float captionSizeScaleFromValue(DWORD raw) {
  switch (raw) {
    case 1:
      return 0.70f;  // FiftyPercent
    case 2:
      return 1.00f;  // OneHundredPercent
    case 3:
      return 1.35f;  // OneHundredFiftyPercent
    case 4:
      return 1.70f;  // TwoHundredPercent
    default:
      return 1.00f;  // Default
  }
}

inline const wchar_t* captionFontFaceForStyle(int fontStyle) {
  switch (fontStyle) {
    case 1:
      return L"Courier New";      // MonospacedWithSerifs
    case 2:
      return L"Times New Roman";  // ProportionalWithSerifs
    case 3:
      return L"Consolas";         // MonospacedWithoutSerifs
    case 4:
      return L"Arial";            // ProportionalWithoutSerifs
    case 5:
      return L"Comic Sans MS";    // Casual
    case 6:
      return L"Segoe Script";     // Cursive
    case 7:
      return L"Arial";            // SmallCapitals
    default:
      return L"Arial";            // Default
  }
}

inline bool readClosedCaptionDword(const wchar_t* valueName, DWORD* outValue) {
  if (!valueName || !outValue) return false;
  HKEY hKey = nullptr;
  constexpr const wchar_t* kKeyPath =
      L"Software\\Microsoft\\Windows\\CurrentVersion\\ClosedCaptioning";
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, KEY_READ, &hKey) !=
      ERROR_SUCCESS) {
    return false;
  }

  DWORD type = 0;
  DWORD value = 0;
  DWORD size = sizeof(value);
  LONG result = RegQueryValueExW(hKey, valueName, nullptr, &type,
                                 reinterpret_cast<LPBYTE>(&value), &size);
  RegCloseKey(hKey);
  if (result != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value)) {
    return false;
  }
  *outValue = value;
  return true;
}

inline CaptionStyleProfile getWindowsCaptionStyleProfile() {
  static std::mutex cacheMutex;
  static ULONGLONG lastRefreshMs = 0;
  static CaptionStyleProfile cached{};

  const ULONGLONG nowMs = GetTickCount64();
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (lastRefreshMs != 0 && nowMs - lastRefreshMs < 1200) {
      return cached;
    }

    CaptionStyleProfile style{};
    DWORD raw = 0;

    if (readClosedCaptionDword(L"CaptionColor", &raw)) {
      captionColorFromValue(raw, 255, 255, 255, &style.textR, &style.textG,
                            &style.textB);
    }
    if (readClosedCaptionDword(L"CaptionOpacity", &raw)) {
      style.textAlpha = captionOpacityFromValue(raw, style.textAlpha);
    }
    if (readClosedCaptionDword(L"BackgroundColor", &raw)) {
      captionColorFromValue(raw, 0, 0, 0, &style.backgroundR,
                            &style.backgroundG, &style.backgroundB);
    }
    if (readClosedCaptionDword(L"BackgroundOpacity", &raw)) {
      style.backgroundAlpha = captionOpacityFromValue(raw, style.backgroundAlpha);
    }
    if (readClosedCaptionDword(L"CaptionSize", &raw)) {
      style.sizeScale = captionSizeScaleFromValue(raw);
    }
    if (readClosedCaptionDword(L"FontStyle", &raw)) {
      style.fontStyle = static_cast<int>(raw);
    }
    if (readClosedCaptionDword(L"FontEffect", &raw)) {
      style.fontEffect = static_cast<int>(raw);
    }

    style.textAlpha = std::clamp(style.textAlpha, 0.0f, 1.0f);
    style.backgroundAlpha = std::clamp(style.backgroundAlpha, 0.0f, 1.0f);
    style.sizeScale = std::clamp(style.sizeScale, 0.60f, 2.0f);

    cached = style;
    lastRefreshMs = nowMs;
  }
  return cached;
}
