#include "terminal_cell_metrics.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace {

bool validCellSize(const TerminalCellPixelSize& size) {
  return size.width > 0.0 && size.height > 0.0;
}

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
                               static_cast<double>(height)};
}

TerminalCellPixelSize requestTerminalTextAreaSize(HANDLE input, HANDLE output,
                                                  int columns, int rows,
                                                  DWORD timeoutMs) {
  if (!writeQuery(output, 14)) return {};

  int height = 0;
  int width = 0;
  if (!readWindowOpReport(input, 4, timeoutMs, height, width)) return {};
  return TerminalCellPixelSize{static_cast<double>(width) / columns,
                               static_cast<double>(height) / rows};
}

}  // namespace

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

  TerminalCellPixelSize size =
      requestTerminalTextAreaSize(input, output, columns, rows, timeoutMs);
  if (!validCellSize(size)) {
    size = requestTerminalCellSize(input, output, timeoutMs);
  }

  if (haveInputMode) {
    SetConsoleMode(input, originalInputMode);
  }
  return size;
}

TerminalCellPixelSize queryConsoleFontCellPixelSize(HANDLE output) {
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
                               static_cast<double>(font.dwFontSize.Y)};
}
