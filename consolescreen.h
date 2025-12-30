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
#include <filesystem>
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

struct Breadcrumb {
  int startX = 0;
  int endX = 0;
  std::filesystem::path path;
};

struct BreadcrumbLine {
  std::string text;
  std::vector<Breadcrumb> crumbs;
};

BreadcrumbLine buildBreadcrumbLine(const std::filesystem::path& dir, int width);
int breadcrumbIndexAt(const BreadcrumbLine& line, int x, int y, int lineY);
bool hitTestBreadcrumb(const BreadcrumbLine& line, int x, int y, int lineY, std::filesystem::path* outPath);

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
  void setFastOutput(bool enabled);
  bool fastOutput() const;
  void draw();

 private:
  void drawFast();

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
  bool fastOutput_ = false;
  std::vector<CHAR_INFO> fastBuffer_;
  std::vector<Cell> buffer_;
  std::wstring drawBuffer_;
};

#endif
