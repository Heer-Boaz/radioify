#include "terminal_cell_metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <string>
#include <vector>

namespace {

bool validCellSize(const TerminalCellPixelSize& size) {
  return size.width > 0.0 && size.height > 0.0;
}

struct GlyphInkBounds {
  int minX = 0;
  int minY = 0;
  int maxX = 0;
  int maxY = 0;
  bool valid = false;

  double centerX() const {
    return (static_cast<double>(minX) + static_cast<double>(maxX) + 1.0) *
           0.5;
  }

  double centerY() const {
    return (static_cast<double>(minY) + static_cast<double>(maxY) + 1.0) *
           0.5;
  }
};

bool writeQuery(HANDLE output, int operation) {
  wchar_t seq[8] = {L'\x1b', L'[', 0, 0, 0, L't', 0, 0};
  int pos = 2;
  if (operation >= 10) {
    seq[pos++] = static_cast<wchar_t>(L'0' + operation / 10);
  }
  seq[pos++] = static_cast<wchar_t>(L'0' + operation % 10);
  seq[pos++] = L't';
  DWORD written = 0;
  return WriteConsoleW(output, seq, static_cast<DWORD>(pos), &written,
                       nullptr) &&
         written == static_cast<DWORD>(pos);
}

bool parseUnsigned(const std::wstring& text, size_t& offset, int& out) {
  if (offset >= text.size() || !iswdigit(text[offset])) return false;
  int value = 0;
  while (offset < text.size() && iswdigit(text[offset])) {
    value = value * 10 + static_cast<int>(text[offset] - L'0');
    ++offset;
  }
  out = value;
  return true;
}

bool parseWindowOpReport(const std::wstring& text, int expectedOperation,
                         int& outHeight, int& outWidth, size_t* outStart,
                         size_t* outEnd) {
  for (size_t i = 0; i + 2 < text.size(); ++i) {
    if (text[i] != L'\x1b' || text[i + 1] != L'[') continue;

    size_t offset = i + 2;
    int operation = 0;
    int height = 0;
    int width = 0;
    if (!parseUnsigned(text, offset, operation)) continue;
    if (offset >= text.size() || text[offset++] != L';') continue;
    if (!parseUnsigned(text, offset, height)) continue;
    if (offset >= text.size() || text[offset++] != L';') continue;
    if (!parseUnsigned(text, offset, width)) continue;
    if (offset >= text.size() || text[offset] != L't') continue;
    if (operation != expectedOperation || width <= 0 || height <= 0) continue;

    outHeight = height;
    outWidth = width;
    if (outStart) *outStart = i;
    if (outEnd) *outEnd = offset + 1;
    return true;
  }
  return false;
}

void restoreInputRecords(HANDLE input,
                         const std::vector<INPUT_RECORD>& records,
                         const std::vector<size_t>& responseCharRecords,
                         size_t responseStart, size_t responseEnd) {
  if (records.empty()) return;

  std::vector<bool> drop(records.size(), false);
  const size_t responseLimit =
      std::min(responseEnd, responseCharRecords.size());
  for (size_t i = responseStart; i < responseLimit; ++i) {
    const size_t recordIndex = responseCharRecords[i];
    if (recordIndex < drop.size()) {
      drop[recordIndex] = true;
    }
  }

  std::vector<INPUT_RECORD> restore;
  restore.reserve(records.size());
  for (size_t i = 0; i < records.size(); ++i) {
    if (!drop[i]) restore.push_back(records[i]);
  }
  if (restore.empty()) return;

  DWORD written = 0;
  WriteConsoleInputW(input, restore.data(),
                     static_cast<DWORD>(restore.size()), &written);
}

bool readWindowOpReport(HANDLE input, int expectedOperation, DWORD timeoutMs,
                        int& outHeight, int& outWidth) {
  std::wstring response;
  response.reserve(32);
  std::vector<INPUT_RECORD> records;
  std::vector<size_t> responseCharRecords;
  const ULONGLONG deadline = GetTickCount64() + timeoutMs;

  while (GetTickCount64() < deadline) {
    const ULONGLONG now = GetTickCount64();
    DWORD waitMs = static_cast<DWORD>(
        std::min<ULONGLONG>(deadline - now, static_cast<ULONGLONG>(10)));
    if (WaitForSingleObject(input, waitMs) != WAIT_OBJECT_0) continue;

    INPUT_RECORD record{};
    DWORD read = 0;
    if (!ReadConsoleInputW(input, &record, 1, &read) || read == 0) {
      continue;
    }
    records.push_back(record);
    if (record.EventType != KEY_EVENT) continue;
    const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
    if (!key.bKeyDown || key.uChar.UnicodeChar == 0) continue;

    responseCharRecords.push_back(records.size() - 1);
    response.push_back(key.uChar.UnicodeChar);
    size_t responseStart = 0;
    size_t responseEnd = 0;
    if (parseWindowOpReport(response, expectedOperation, outHeight, outWidth,
                            &responseStart, &responseEnd)) {
      restoreInputRecords(input, records, responseCharRecords, responseStart,
                          responseEnd);
      return true;
    }
  }
  restoreInputRecords(input, records, responseCharRecords, 0, 0);
  return false;
}

TerminalCellPixelSize requestTerminalCellSize(HANDLE input, HANDLE output,
                                              DWORD timeoutMs) {
  if (!writeQuery(output, 16)) return {};

  int height = 0;
  int width = 0;
  if (!readWindowOpReport(input, 6, timeoutMs, height, width)) return {};
  return TerminalCellPixelSize{static_cast<double>(width),
                               static_cast<double>(height),
                               TerminalCellMetricSource::Csi16CellSize};
}

TerminalCellPixelSize requestTerminalTextAreaSize(HANDLE input, HANDLE output,
                                                  int columns, int rows,
                                                  DWORD timeoutMs) {
  if (!writeQuery(output, 14)) return {};

  int height = 0;
  int width = 0;
  if (!readWindowOpReport(input, 4, timeoutMs, height, width)) return {};
  return TerminalCellPixelSize{static_cast<double>(width) / columns,
                               static_cast<double>(height) / rows,
                               TerminalCellMetricSource::Csi14TextArea};
}

TerminalCellPixelSize queryConsoleFontCellPixelSizeImpl(HANDLE output) {
  if (output == INVALID_HANDLE_VALUE) {
    return {};
  }

  CONSOLE_FONT_INFOEX font{};
  font.cbSize = sizeof(font);
  if (!GetCurrentConsoleFontEx(output, FALSE, &font)) {
    return {};
  }
  if (font.dwFontSize.X <= 0 || font.dwFontSize.Y <= 0) {
    return {};
  }
  return TerminalCellPixelSize{static_cast<double>(font.dwFontSize.X),
                               static_cast<double>(font.dwFontSize.Y),
                               TerminalCellMetricSource::Win32ConsoleFont};
}

bool measureGlyphInkBounds(HDC dc, void* pixels, int width, int height,
                           int originX, int originY, wchar_t ch,
                           GlyphInkBounds& out) {
  if (!dc || !pixels || width <= 0 || height <= 0) {
    return false;
  }

  auto* bgra = static_cast<uint32_t*>(pixels);
  std::fill(bgra, bgra + static_cast<size_t>(width) * height, 0u);
  if (!TextOutW(dc, originX, originY, &ch, 1)) {
    return false;
  }

  GlyphInkBounds bounds{};
  for (int y = 0; y < height; ++y) {
    const uint8_t* row =
        reinterpret_cast<const uint8_t*>(bgra + static_cast<size_t>(y) * width);
    for (int x = 0; x < width; ++x) {
      const uint8_t* px = row + static_cast<size_t>(x) * 4u;
      if ((px[0] | px[1] | px[2]) == 0) {
        continue;
      }
      if (!bounds.valid) {
        bounds.minX = bounds.maxX = x;
        bounds.minY = bounds.maxY = y;
        bounds.valid = true;
      } else {
        bounds.minX = std::min(bounds.minX, x);
        bounds.minY = std::min(bounds.minY, y);
        bounds.maxX = std::max(bounds.maxX, x);
        bounds.maxY = std::max(bounds.maxY, y);
      }
    }
  }

  if (!bounds.valid) {
    return false;
  }
  out = bounds;
  return true;
}

}  // namespace

const char* terminalCellMetricSourceLabel(TerminalCellMetricSource source) {
  switch (source) {
    case TerminalCellMetricSource::Csi16CellSize:
      return "csi-16t-cell-size";
    case TerminalCellMetricSource::Win32ConsoleFont:
      return "win32-console-font";
    case TerminalCellMetricSource::Csi14TextArea:
      return "csi-14t-text-area";
    case TerminalCellMetricSource::BrailleGlyphInk:
      return "braille-glyph-ink";
    case TerminalCellMetricSource::None:
      break;
  }
  return "none";
}

TerminalCellPixelSize queryTerminalDirectCellPixelSize(HANDLE input,
                                                       HANDLE output,
                                                       DWORD timeoutMs) {
  if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE) {
    return {};
  }

  DWORD originalInputMode = 0;
  const bool haveInputMode = GetConsoleMode(input, &originalInputMode) != FALSE;
  if (haveInputMode) {
    SetConsoleMode(input, originalInputMode | ENABLE_VIRTUAL_TERMINAL_INPUT);
  }

  TerminalCellPixelSize size = requestTerminalCellSize(input, output, timeoutMs);

  if (haveInputMode) {
    SetConsoleMode(input, originalInputMode);
  }
  return size;
}

TerminalCellPixelSize queryTerminalTextAreaCellPixelSize(HANDLE input,
                                                         HANDLE output,
                                                         int columns,
                                                         int rows,
                                                         DWORD timeoutMs) {
  if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE ||
      columns <= 0 || rows <= 0) {
    return {};
  }

  DWORD originalInputMode = 0;
  const bool haveInputMode = GetConsoleMode(input, &originalInputMode) != FALSE;
  if (haveInputMode) {
    SetConsoleMode(input, originalInputMode | ENABLE_VIRTUAL_TERMINAL_INPUT);
  }

  TerminalCellPixelSize size =
      requestTerminalTextAreaSize(input, output, columns, rows, timeoutMs);

  if (haveInputMode) {
    SetConsoleMode(input, originalInputMode);
  }
  return size;
}

TerminalCellPixelSize queryTerminalCellPixelSize(HANDLE input, HANDLE output,
                                                 int columns, int rows,
                                                 DWORD timeoutMs) {
  if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE) {
    return {};
  }

  DWORD originalInputMode = 0;
  const bool haveInputMode = GetConsoleMode(input, &originalInputMode) != FALSE;
  if (haveInputMode) {
    SetConsoleMode(input, originalInputMode | ENABLE_VIRTUAL_TERMINAL_INPUT);
  }

  TerminalCellPixelSize size = requestTerminalCellSize(input, output, timeoutMs);
  if (!validCellSize(size)) {
    size = queryConsoleFontCellPixelSizeImpl(output);
  }
  if (!validCellSize(size)) {
    // CSI 14t reports the text area, which may include terminal padding.
    // Use it only when neither a direct cell query nor Win32 font metrics work.
    size = requestTerminalTextAreaSize(input, output, columns, rows, timeoutMs);
  }

  if (haveInputMode) {
    SetConsoleMode(input, originalInputMode);
  }
  return size;
}

TerminalCellPixelSize queryConsoleFontCellPixelSize(HANDLE output) {
  return queryConsoleFontCellPixelSizeImpl(output);
}

TerminalCellPixelSize queryBrailleGlyphCellPixelSize(HANDLE output,
                                                     double cellPixelWidth,
                                                     double cellPixelHeight) {
  if (output == INVALID_HANDLE_VALUE || cellPixelWidth <= 0.0 ||
      cellPixelHeight <= 0.0) {
    return {};
  }

  CONSOLE_FONT_INFOEX font{};
  font.cbSize = sizeof(font);
  if (!GetCurrentConsoleFontEx(output, FALSE, &font)) {
    return {};
  }

  int fontPixelHeight = font.dwFontSize.Y;
  if (fontPixelHeight <= 0) {
    fontPixelHeight = static_cast<int>(std::lround(cellPixelHeight));
  }
  if (fontPixelHeight <= 0) {
    return {};
  }

  HDC dc = CreateCompatibleDC(nullptr);
  if (!dc) {
    return {};
  }

  const wchar_t* faceName = font.FaceName[0] ? font.FaceName : L"Cascadia Mono";
  const int weight = font.FontWeight > 0 ? static_cast<int>(font.FontWeight)
                                         : FW_NORMAL;
  HFONT hfont =
      CreateFontW(-fontPixelHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                  ANTIALIASED_QUALITY, FIXED_PITCH | FF_DONTCARE, faceName);
  if (!hfont) {
    DeleteDC(dc);
    return {};
  }

  const int canvasW =
      std::max(32, static_cast<int>(std::ceil(cellPixelWidth * 4.0)));
  const int canvasH =
      std::max(32, static_cast<int>(std::ceil(cellPixelHeight * 4.0)));

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = canvasW;
  bmi.bmiHeader.biHeight = -canvasH;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* pixels = nullptr;
  HBITMAP bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &pixels, nullptr,
                                    0);
  if (!bitmap || !pixels) {
    if (bitmap) {
      DeleteObject(bitmap);
    }
    DeleteObject(hfont);
    DeleteDC(dc);
    return {};
  }

  HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
  HGDIOBJ oldFont = SelectObject(dc, hfont);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));

  const int originX = static_cast<int>(std::ceil(cellPixelWidth));
  const int originY = static_cast<int>(std::ceil(cellPixelHeight));
  GlyphInkBounds dot1;
  GlyphInkBounds dot4;
  GlyphInkBounds dot7;
  const bool ok =
      measureGlyphInkBounds(dc, pixels, canvasW, canvasH, originX, originY,
                            static_cast<wchar_t>(0x2801), dot1) &&
      measureGlyphInkBounds(dc, pixels, canvasW, canvasH, originX, originY,
                            static_cast<wchar_t>(0x2808), dot4) &&
      measureGlyphInkBounds(dc, pixels, canvasW, canvasH, originX, originY,
                            static_cast<wchar_t>(0x2840), dot7);

  if (oldFont) {
    SelectObject(dc, oldFont);
  }
  if (oldBitmap) {
    SelectObject(dc, oldBitmap);
  }
  DeleteObject(bitmap);
  DeleteObject(hfont);
  DeleteDC(dc);

  if (!ok) {
    return {};
  }

  const double columnPitch = std::abs(dot4.centerX() - dot1.centerX());
  const double rowPitch = std::abs(dot7.centerY() - dot1.centerY()) / 3.0;
  const double width = columnPitch * 2.0;
  const double height = rowPitch * 4.0;
  if (width <= 0.0 || height <= 0.0) {
    return {};
  }

  return TerminalCellPixelSize{width, height,
                               TerminalCellMetricSource::BrailleGlyphInk};
}
