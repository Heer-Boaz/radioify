#include "consolescreen.h"

#include <algorithm>
#include <cwchar>

static std::wstring utf8ToWide(const std::string& text) {
  if (text.empty()) return {};
  int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) {
    std::wstring fallback;
    fallback.reserve(text.size());
    for (unsigned char c : text) {
      fallback.push_back(static_cast<wchar_t>(c));
    }
    return fallback;
  }
  std::wstring out(needed, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()), out.data(), needed);
  return out;
}

static bool sameColor(const Color& a, const Color& b) {
  return a.r == b.r && a.g == b.g && a.b == b.b;
}

bool ConsoleScreen::init() {
  out_ = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out_ == INVALID_HANDLE_VALUE) return false;
  if (!GetConsoleMode(out_, &originalMode_)) return false;
  DWORD mode = originalMode_;
  mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT;
  SetConsoleMode(out_, mode);
  CONSOLE_CURSOR_INFO cursor{};
  if (GetConsoleCursorInfo(out_, &cursor)) {
    originalCursor_ = cursor;
    cursor.bVisible = FALSE;
    SetConsoleCursorInfo(out_, &cursor);
  }
  updateSize();
  return true;
}

void ConsoleScreen::restore() {
  if (out_ != INVALID_HANDLE_VALUE) {
    SetConsoleMode(out_, originalMode_);
    if (originalCursor_.dwSize != 0) {
      SetConsoleCursorInfo(out_, &originalCursor_);
    }
  }
}

void ConsoleScreen::updateSize() {
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(out_, &info)) {
    width_ = 80;
    height_ = 25;
  } else {
    width_ = info.srWindow.Right - info.srWindow.Left + 1;
    height_ = info.srWindow.Bottom - info.srWindow.Top + 1;
  }
  if (width_ < 1) width_ = 1;
  if (height_ < 1) height_ = 1;
  if (static_cast<int>(buffer_.size()) != width_ * height_) {
    buffer_.assign(static_cast<size_t>(width_ * height_), {});
  }
}

int ConsoleScreen::width() const { return width_; }
int ConsoleScreen::height() const { return height_; }

void ConsoleScreen::clear(const Style& style) {
  Cell cell{};
  cell.ch = ' ';
  cell.fg = style.fg;
  cell.bg = style.bg;
  std::fill(buffer_.begin(), buffer_.end(), cell);
}

void ConsoleScreen::writeText(int x, int y, const std::string& text, const Style& style) {
  if (y < 0 || y >= height_) return;
  if (x >= width_) return;
  int pos = y * width_ + std::max(0, x);
  int start = std::max(0, x);
  int maxLen = width_ - start;
  std::wstring wide = utf8ToWide(text);
  int limit = std::min(maxLen, static_cast<int>(wide.size()));
  for (int i = 0; i < limit; ++i) {
    wchar_t ch = wide[static_cast<size_t>(i)];
    if (i == limit - 1 && ch >= 0xD800 && ch <= 0xDBFF) {
      break;
    }
    buffer_[pos + i].ch = ch;
    buffer_[pos + i].fg = style.fg;
    buffer_[pos + i].bg = style.bg;
  }
}

void ConsoleScreen::writeRun(int x, int y, int len, wchar_t ch, const Style& style) {
  if (y < 0 || y >= height_) return;
  if (x >= width_) return;
  int start = std::max(0, x);
  int maxLen = std::min(len, width_ - start);
  int pos = y * width_ + start;
  for (int i = 0; i < maxLen; ++i) {
    buffer_[pos + i].ch = ch;
    buffer_[pos + i].fg = style.fg;
    buffer_[pos + i].bg = style.bg;
  }
}

void ConsoleScreen::writeChar(int x, int y, wchar_t ch, const Style& style) {
  if (y < 0 || y >= height_) return;
  if (x < 0 || x >= width_) return;
  int pos = y * width_ + x;
  buffer_[pos].ch = ch;
  buffer_[pos].fg = style.fg;
  buffer_[pos].bg = style.bg;
}

void ConsoleScreen::draw() {
  if (out_ == INVALID_HANDLE_VALUE) return;
  std::wstring out;
  out.reserve(static_cast<size_t>(width_ * height_ * 2));
  out.append(L"\x1b[H");
  Color curFg{};
  Color curBg{};
  bool hasColor = false;

  auto appendColor = [&](const Color& fg, const Color& bg) {
    wchar_t buf[64];
    std::swprintf(
      buf,
      static_cast<int>(sizeof(buf) / sizeof(*buf)),
      L"\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um",
      static_cast<unsigned>(fg.r),
      static_cast<unsigned>(fg.g),
      static_cast<unsigned>(fg.b),
      static_cast<unsigned>(bg.r),
      static_cast<unsigned>(bg.g),
      static_cast<unsigned>(bg.b)
    );
    out.append(buf);
  };

  for (int y = 0; y < height_; ++y) {
    for (int x = 0; x < width_; ++x) {
      const Cell& cell = buffer_[y * width_ + x];
      if (!hasColor || !sameColor(cell.fg, curFg) || !sameColor(cell.bg, curBg)) {
        appendColor(cell.fg, cell.bg);
        curFg = cell.fg;
        curBg = cell.bg;
        hasColor = true;
      }
      out.push_back(cell.ch ? cell.ch : L' ');
    }
    if (y < height_ - 1) {
      out.append(L"\x1b[0m\r\n");
      hasColor = false;
    }
  }
  out.append(L"\x1b[0m");

  DWORD written = 0;
  WriteConsoleW(out_, out.c_str(), static_cast<DWORD>(out.size()), &written, nullptr);
}
