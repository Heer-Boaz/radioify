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

struct ScreenCell {
  wchar_t ch = L' ';
  uint8_t cellWidth = 1;
  bool continuation = false;
  Color fg{255, 255, 255};
  Color bg{0, 0, 0};
};

BreadcrumbLine buildBreadcrumbLine(const std::filesystem::path& dir, int width);
int breadcrumbIndexAt(const BreadcrumbLine& line, int x, int y, int lineY);
bool hitTestBreadcrumb(const BreadcrumbLine& line, int x, int y, int lineY, std::filesystem::path* outPath);
HANDLE browserThumbnailWakeHandle();
bool consumeBrowserThumbnailWake();

class ConsoleScreen {
 public:
  bool init();
  void restore();
  void updateSize();
  void setVirtualSize(int width, int height);
  void clearVirtualSize();
  int width() const;
  int height() const;
  int cellPixelWidth() const;
  int cellPixelHeight() const;
  void clear(const Style& style);
  void writeText(int x, int y, const std::string& text, const Style& style);
  void writeRun(int x, int y, int len, wchar_t ch, const Style& style);
  void writeChar(int x, int y, wchar_t ch, const Style& style);
  void setFastOutput(bool enabled);
  void setAlwaysFullRedraw(bool enabled);
  bool fastOutput() const;
  bool snapshot(std::vector<ScreenCell>& outCells, int& outWidth,
                int& outHeight) const;
  void draw();

 private:
  void syncScreenBufferToWindow();
  void updateCellPixelSize();
  void applySize(int width, int height);
  void drawFast();
  bool writeOutput(const std::wstring& text);
  void reportWriteError(const wchar_t* context);

  struct Cell {
    wchar_t glyph[2]{L' ', L'\0'};
    uint8_t glyphLen = 1;
    uint8_t cellWidth = 1;
    bool continuation = false;
    Color fg{255, 255, 255};
    Color bg{0, 0, 0};
  };

  static void setCellSpace(Cell& cell, const Style& style);
  static void setCellGlyph(Cell& cell, const wchar_t* glyph, uint8_t glyphLen,
                           uint8_t cellWidth, bool continuation,
                           const Style& style);
  static bool sameCell(const Cell& a, const Cell& b);
  static void appendCellGlyph(std::wstring& out, const Cell& cell);

  HANDLE out_ = INVALID_HANDLE_VALUE;
  DWORD originalMode_ = 0;
  CONSOLE_CURSOR_INFO originalCursor_{};
  UINT originalOutputCp_ = 0;
  int width_ = 80;
  int height_ = 25;
  int cellPixelWidth_ = 0;
  int cellPixelHeight_ = 0;
  bool fastOutput_ = false;
  bool virtualSize_ = false;
  bool alwaysFullRedraw_ = false;
  bool hasPrev_ = false;
  bool altScreen_ = false;
  bool useUtf8Output_ = true;
  bool outputFailed_ = false;
  bool outputErrorReported_ = false;
  DWORD outputError_ = 0;
  bool clearOnNextFullRedraw_ = true;
  std::vector<CHAR_INFO> fastBuffer_;
  std::vector<Cell> buffer_;
  std::vector<Cell> prevBuffer_;
  std::wstring drawBuffer_;
  std::string drawUtf8Buffer_;
};

#endif
