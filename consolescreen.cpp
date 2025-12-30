#include "consolescreen.h"

#include <algorithm>
#include <array>
#include <cwchar>
#include <limits>

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

static std::string pathToUtf8(const std::filesystem::path& p) {
#ifdef _WIN32
  auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
#else
  return p.string();
#endif
}

static size_t utf8Next(const std::string& s, size_t i) {
  if (i >= s.size()) return s.size();
  unsigned char c = static_cast<unsigned char>(s[i]);
  if ((c & 0x80) == 0x00) return i + 1;
  if ((c & 0xE0) == 0xC0) return std::min(s.size(), i + 2);
  if ((c & 0xF0) == 0xE0) return std::min(s.size(), i + 3);
  if ((c & 0xF8) == 0xF0) return std::min(s.size(), i + 4);
  return i + 1;
}

static int utf8CodepointCount(const std::string& s) {
  int count = 0;
  for (size_t i = 0; i < s.size();) {
    i = utf8Next(s, i);
    count++;
  }
  return count;
}

static std::string utf8Take(const std::string& s, int count) {
  if (count <= 0) return "";
  size_t i = 0;
  int c = 0;
  for (; i < s.size() && c < count; ++c) {
    i = utf8Next(s, i);
  }
  return s.substr(0, i);
}

static std::string fitLine(const std::string& s, int width) {
  if (width <= 0) return "";
  int count = utf8CodepointCount(s);
  if (count <= width) return s;
  if (width <= 1) return utf8Take(s, width);
  return utf8Take(s, width - 1) + "~";
}

static bool sameColor(const Color& a, const Color& b) {
  return a.r == b.r && a.g == b.g && a.b == b.b;
}

struct PaletteEntry {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

constexpr std::array<PaletteEntry, 16> kConsolePalette = {{
    {0, 0, 0},
    {0, 0, 128},
    {0, 128, 0},
    {0, 128, 128},
    {128, 0, 0},
    {128, 0, 128},
    {128, 128, 0},
    {192, 192, 192},
    {128, 128, 128},
    {0, 0, 255},
    {0, 255, 0},
    {0, 255, 255},
    {255, 0, 0},
    {255, 0, 255},
    {255, 255, 0},
    {255, 255, 255},
}};

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t r5 = static_cast<uint16_t>(r >> 3);
  uint16_t g6 = static_cast<uint16_t>(g >> 2);
  uint16_t b5 = static_cast<uint16_t>(b >> 3);
  return static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
}

const std::array<uint8_t, 65536> kConsoleColorFrom565 = []() {
  std::array<uint8_t, 65536> table{};
  for (int i = 0; i < 65536; ++i) {
    uint8_t r5 = static_cast<uint8_t>((i >> 11) & 0x1F);
    uint8_t g6 = static_cast<uint8_t>((i >> 5) & 0x3F);
    uint8_t b5 = static_cast<uint8_t>(i & 0x1F);
    uint8_t r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    uint8_t g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
    uint8_t b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
    int best = 0;
    int bestDist = std::numeric_limits<int>::max();
    for (int p = 0; p < static_cast<int>(kConsolePalette.size()); ++p) {
      int dr = static_cast<int>(r) - static_cast<int>(kConsolePalette[p].r);
      int dg = static_cast<int>(g) - static_cast<int>(kConsolePalette[p].g);
      int db = static_cast<int>(b) - static_cast<int>(kConsolePalette[p].b);
      int dist = dr * dr + dg * dg + db * db;
      if (dist < bestDist) {
        bestDist = dist;
        best = p;
      }
    }
    table[static_cast<size_t>(i)] = static_cast<uint8_t>(best);
  }
  return table;
}();

inline uint8_t consoleColorIndex(const Color& c) {
  return kConsoleColorFrom565[rgb565(c.r, c.g, c.b)];
}

inline void appendByte(std::wstring& out, unsigned value) {
  if (value >= 100) {
    out.push_back(static_cast<wchar_t>(L'0' + (value / 100)));
    value %= 100;
    out.push_back(static_cast<wchar_t>(L'0' + (value / 10)));
    out.push_back(static_cast<wchar_t>(L'0' + (value % 10)));
    return;
  }
  if (value >= 10) {
    out.push_back(static_cast<wchar_t>(L'0' + (value / 10)));
    out.push_back(static_cast<wchar_t>(L'0' + (value % 10)));
    return;
  }
  out.push_back(static_cast<wchar_t>(L'0' + value));
}

inline void appendColorSeq(std::wstring& out, const Color& c, bool fg) {
  if (fg) {
    out.append(L"\x1b[38;2;");
  } else {
    out.append(L"\x1b[48;2;");
  }
  appendByte(out, c.r);
  out.push_back(L';');
  appendByte(out, c.g);
  out.push_back(L';');
  appendByte(out, c.b);
  out.push_back(L'm');
}

BreadcrumbLine buildBreadcrumbLine(const std::filesystem::path& dir, int width) {
  BreadcrumbLine line;
  const std::string prefix = "  Path: ";
  const int prefixLen = utf8CodepointCount(prefix);
  if (width <= 0) return line;
  if (prefixLen >= width) {
    line.text = fitLine(prefix, width);
    return line;
  }

  struct Item {
    std::string label;
    std::filesystem::path path;
  };
  std::vector<Item> items;
  std::filesystem::path cur;
  std::filesystem::path root = dir.root_path();
#ifdef _WIN32
  if (!root.empty()) {
    std::string rootLabel = pathToUtf8(dir.root_name());
    if (rootLabel.empty()) rootLabel = pathToUtf8(root);
    if (!rootLabel.empty() && (rootLabel.back() == '\\' || rootLabel.back() == '/')) {
      rootLabel.pop_back();
    }
    if (rootLabel.empty()) rootLabel = "\\";
    cur = root;
    items.push_back(Item{rootLabel, cur});
  }
#else
  if (!root.empty()) {
    cur = root;
    items.push_back(Item{"/", cur});
  }
#endif
  for (const auto& part : dir.relative_path()) {
    cur /= part;
    items.push_back(Item{pathToUtf8(part), cur});
  }
  if (items.empty()) {
    items.push_back(Item{pathToUtf8(dir), dir});
  }

  const std::string sep = " > ";
  const int sepLen = utf8CodepointCount(sep);
  const std::string ellipsis = "... > ";
  const int ellipsisLen = utf8CodepointCount(ellipsis);

  const int avail = width - prefixLen;
  std::vector<Item> visibleRev;
  int used = 0;
  int index = static_cast<int>(items.size()) - 1;
  for (; index >= 0; --index) {
    int labelLen = utf8CodepointCount(items[index].label);
    int add = labelLen + (visibleRev.empty() ? 0 : sepLen);
    if (used + add <= avail) {
      visibleRev.push_back(items[index]);
      used += add;
    } else {
      break;
    }
  }

  bool skipped = index >= 0;
  if (visibleRev.empty()) {
    Item last = items.back();
    last.label = fitLine(last.label, avail);
    visibleRev.push_back(last);
    used = utf8CodepointCount(last.label);
    skipped = items.size() > 1;
  }

  int ellipsisUse = (skipped && used + ellipsisLen <= avail) ? ellipsisLen : 0;
  std::vector<Item> visible(visibleRev.rbegin(), visibleRev.rend());

  std::string text = prefix;
  int x = prefixLen;
  if (ellipsisUse) {
    text += ellipsis;
    x += ellipsisLen;
  }
  for (size_t i = 0; i < visible.size(); ++i) {
    Item item = visible[i];
    int remaining = width - x;
    if (remaining <= 0) break;
    int labelLen = utf8CodepointCount(item.label);
    if (labelLen > remaining) {
      item.label = fitLine(item.label, remaining);
      labelLen = utf8CodepointCount(item.label);
    }
    Breadcrumb crumb;
    crumb.startX = x;
    crumb.endX = x + labelLen;
    crumb.path = item.path;
    line.crumbs.push_back(crumb);

    text += item.label;
    x += labelLen;
    if (i + 1 < visible.size()) {
      if (x + sepLen > width) break;
      text += sep;
      x += sepLen;
    }
  }
  line.text = text;
  if (line.text.empty()) {
    std::string fallback = fitLine(pathToUtf8(dir), width - prefixLen);
    line.text = prefix + fallback;
  }
  if (line.crumbs.empty()) {
    int total = utf8CodepointCount(line.text);
    if (total > prefixLen) {
      Breadcrumb crumb;
      crumb.startX = prefixLen;
      crumb.endX = std::min(width, total);
      crumb.path = dir;
      line.crumbs.push_back(crumb);
    }
  }
  return line;
}

bool hitTestBreadcrumb(const BreadcrumbLine& line, int x, int y, int lineY, std::filesystem::path* outPath) {
  int index = breadcrumbIndexAt(line, x, y, lineY);
  if (index < 0) return false;
  if (outPath) *outPath = line.crumbs[static_cast<size_t>(index)].path;
  return true;
}

int breadcrumbIndexAt(const BreadcrumbLine& line, int x, int y, int lineY) {
  if (y != lineY) return -1;
  for (size_t i = 0; i < line.crumbs.size(); ++i) {
    const auto& crumb = line.crumbs[i];
    if (x >= crumb.startX && x < crumb.endX) {
      return static_cast<int>(i);
    }
  }
  return -1;
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

void ConsoleScreen::setFastOutput(bool enabled) { fastOutput_ = enabled; }

bool ConsoleScreen::fastOutput() const { return fastOutput_; }

void ConsoleScreen::drawFast() {
  if (out_ == INVALID_HANDLE_VALUE) return;
  size_t needed = static_cast<size_t>(width_ * height_);
  if (fastBuffer_.size() != needed) {
    fastBuffer_.resize(needed);
  }
  for (size_t i = 0; i < needed; ++i) {
    const Cell& cell = buffer_[i];
    CHAR_INFO& outCell = fastBuffer_[i];
    outCell.Char.UnicodeChar = cell.ch ? cell.ch : L' ';
    uint8_t fg = consoleColorIndex(cell.fg);
    uint8_t bg = consoleColorIndex(cell.bg);
    outCell.Attributes = static_cast<WORD>(fg | (bg << 4));
  }
  COORD bufSize{static_cast<SHORT>(width_), static_cast<SHORT>(height_)};
  COORD bufPos{0, 0};
  SMALL_RECT region{0, 0, static_cast<SHORT>(width_ - 1),
                    static_cast<SHORT>(height_ - 1)};
  WriteConsoleOutputW(out_, fastBuffer_.data(), bufSize, bufPos, &region);
}

void ConsoleScreen::draw() {
  if (out_ == INVALID_HANDLE_VALUE) return;
  if (fastOutput_) {
    drawFast();
    return;
  }
  std::wstring& out = drawBuffer_;
  out.clear();
  out.reserve(static_cast<size_t>(width_ * height_ * 2 + width_ * 12));
  out.append(L"\x1b[H");
  Color curFg{};
  Color curBg{};
  bool hasColor = false;

  for (int y = 0; y < height_; ++y) {
    for (int x = 0; x < width_; ++x) {
      const Cell& cell = buffer_[y * width_ + x];
      bool fgDiff = !hasColor || !sameColor(cell.fg, curFg);
      bool bgDiff = !hasColor || !sameColor(cell.bg, curBg);
      if (fgDiff) {
        appendColorSeq(out, cell.fg, true);
      }
      if (bgDiff) {
        appendColorSeq(out, cell.bg, false);
      }
      if (fgDiff || bgDiff) {
        curFg = cell.fg;
        curBg = cell.bg;
        hasColor = true;
      }
      out.push_back(cell.ch ? cell.ch : L' ');
    }
    if (y < height_ - 1) {
      out.append(L"\r\n");
    }
  }
  out.append(L"\x1b[0m");

  DWORD written = 0;
  WriteConsoleW(out_, out.c_str(), static_cast<DWORD>(out.size()), &written, nullptr);
}
