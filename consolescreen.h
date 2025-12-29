#ifndef CONSOLESCREEN_H
#define CONSOLESCREEN_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

struct Color {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
};

struct Style {
  Color fg;
  Color bg;
};

class ConsoleScreen {
 public:
  bool init();
  void restore();
  void updateSize();
  int width() const;
  int height() const;
  void clear(const Style& style);
  void writeText(int x, int y, const std::string& text, const Style& style);
  void writeRun(int x, int y, int len, wchar_t ch, const Style& style);
  void writeChar(int x, int y, wchar_t ch, const Style& style);
  void draw();

 private:
  struct Cell {
    wchar_t ch = L' ';
    Color fg{255, 255, 255};
    Color bg{0, 0, 0};
  };

  HANDLE out_ = INVALID_HANDLE_VALUE;
  DWORD originalMode_ = 0;
  CONSOLE_CURSOR_INFO originalCursor_{};
  int width_ = 80;
  int height_ = 25;
  std::vector<Cell> buffer_;
};

#endif
