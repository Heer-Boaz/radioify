#include "consolescreen.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "asciiart.h"
#include "browser_grid_index.h"
#include "consoleinput.h"
#include "playback/playback_media_artwork_catalog.h"
#include "runtime_helpers.h"
#include "terminal_font.h"
#include "ui_helpers.h"
#include "unicode_display_width.h"
#include "videodecoder.h"

static bool wideToUtf8(const std::wstring& text, std::string& out) {
  if (text.empty()) {
    out.clear();
    return true;
  }
  int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                   static_cast<int>(text.size()), nullptr, 0,
                                   nullptr, nullptr);
  if (needed <= 0) return false;
  out.resize(static_cast<size_t>(needed));
  int written = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
      static_cast<int>(text.size()), out.data(), needed, nullptr, nullptr);
  if (written <= 0) return false;
  if (written != needed) out.resize(static_cast<size_t>(written));
  return true;
}

static bool decodeUtf8Codepoint(std::string_view text, size_t* offset,
                                char32_t* outCodepoint, size_t* outStart,
                                size_t* outEnd) {
  if (!offset || *offset >= text.size()) return false;
  size_t start = *offset;
  unsigned char lead = static_cast<unsigned char>(text[start]);
  char32_t codepoint = 0xFFFD;
  size_t length = 1;
  if (lead < 0x80) {
    codepoint = lead;
  } else if ((lead & 0xE0) == 0xC0 && start + 1 < text.size()) {
    codepoint = (lead & 0x1F) << 6;
    codepoint |= static_cast<unsigned char>(text[start + 1]) & 0x3F;
    length = 2;
  } else if ((lead & 0xF0) == 0xE0 && start + 2 < text.size()) {
    codepoint = (lead & 0x0F) << 12;
    codepoint |=
        (static_cast<unsigned char>(text[start + 1]) & 0x3F) << 6;
    codepoint |= static_cast<unsigned char>(text[start + 2]) & 0x3F;
    length = 3;
  } else if ((lead & 0xF8) == 0xF0 && start + 3 < text.size()) {
    codepoint = (lead & 0x07) << 18;
    codepoint |=
        (static_cast<unsigned char>(text[start + 1]) & 0x3F) << 12;
    codepoint |=
        (static_cast<unsigned char>(text[start + 2]) & 0x3F) << 6;
    codepoint |= static_cast<unsigned char>(text[start + 3]) & 0x3F;
    length = 4;
  }
  *offset = start + length;
  if (outCodepoint) *outCodepoint = codepoint;
  if (outStart) *outStart = start;
  if (outEnd) *outEnd = start + length;
  return true;
}

static uint8_t encodeUtf16(char32_t codepoint, wchar_t glyph[2]) {
  if (codepoint > 0x10FFFF) codepoint = 0xFFFD;
  if (codepoint <= 0xFFFF) {
    glyph[0] = static_cast<wchar_t>(codepoint);
    glyph[1] = L'\0';
    return 1;
  }
  codepoint -= 0x10000;
  glyph[0] = static_cast<wchar_t>(0xD800 + (codepoint >> 10));
  glyph[1] = static_cast<wchar_t>(0xDC00 + (codepoint & 0x3FF));
  return 2;
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

inline void appendNumber(std::wstring& out, int value) {
  if (value <= 0) {
    out.push_back(L'0');
    return;
  }
  wchar_t buf[16];
  const int bufSize = static_cast<int>(sizeof(buf) / sizeof(buf[0]));
  int len = 0;
  while (value > 0 && len < bufSize) {
    buf[len++] = static_cast<wchar_t>(L'0' + (value % 10));
    value /= 10;
  }
  for (int i = len - 1; i >= 0; --i) {
    out.push_back(buf[i]);
  }
}

inline void appendCursorPos(std::wstring& out, int x, int y) {
  out.append(L"\x1b[");
  appendNumber(out, y);
  out.push_back(L';');
  appendNumber(out, x);
  out.push_back(L'H');
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

struct ThumbnailCell {
  wchar_t ch = L' ';
  Color fg{255, 255, 255};
  Color bg{0, 0, 0};
  bool hasBg = false;
};

struct Thumbnail {
  bool ok = false;
  int width = 0;
  int height = 0;
  std::vector<ThumbnailCell> cells;
};

enum class ThumbState {
  Pending,
  Ready,
  Failed,
};

struct ThumbEntry {
  ThumbState state = ThumbState::Pending;
  std::shared_ptr<Thumbnail> thumb;
};

struct ThumbJob {
  std::string key;
  std::filesystem::path path;
  bool isImage = false;
  bool isVideo = false;
  bool isAudio = false;
  int trackIndex = -1;
  int width = 0;
  int height = 0;
  uint64_t generation = 0;
};

struct ThumbCacheState {
  ThumbCacheState() {
    thumbnailReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }

  ~ThumbCacheState() {
    if (thumbnailReadyEvent) {
      CloseHandle(thumbnailReadyEvent);
      thumbnailReadyEvent = nullptr;
    }
  }

  int thumbW = 0;
  int thumbH = 0;
  uint64_t generation = 1;
  std::unordered_map<std::string, ThumbEntry> entries;
  std::deque<ThumbJob> queue;
  std::mutex mutex;
  std::condition_variable cv;
  bool workerStarted = false;
  HANDLE thumbnailReadyEvent = nullptr;
};

static ThumbCacheState& thumbCache() {
  static ThumbCacheState cache;
  return cache;
}

static std::string thumbnailCacheKey(const FileEntry& entry) {
  std::string key = toUtf8String(entry.path);
  if (entry.trackIndex >= 0) {
    key += "#track=" + std::to_string(entry.trackIndex);
  }
  return key;
}

static std::string fitName(const std::string& name, int colWidth) {
  int maxLen = colWidth - 2;
  if (maxLen <= 1) return name.empty() ? " " : utf8TakeDisplayWidth(name, 1);
  if (utf8DisplayWidth(name) <= maxLen) return name;
  return utf8TakeDisplayWidth(name, maxLen - 1) + "~";
}

static void drawCenteredText(ConsoleScreen& screen, int x, int y, int width,
                             int height, const std::string& text,
                             const Style& style) {
  if (width <= 0 || height <= 0 || text.empty()) return;
  int textWidth = utf8DisplayWidth(text);
  if (textWidth <= 0) return;
  int cx = x + std::max(0, (width - textWidth) / 2);
  int cy = y + height / 2;
  screen.writeText(cx, cy, text, style);
}

static void drawScrollBar(ConsoleScreen& screen, int x, int width, int top,
                          int height, int scrollRow, int totalRows,
                          int visibleRows, const Style& trackStyle,
                          const Style& thumbStyle) {
  if (x < 0 || width <= 0 || height <= 0 || totalRows <= visibleRows) return;
  int maxScroll = std::max(0, totalRows - visibleRows);
  if (maxScroll <= 0) return;
  int barHeight = height;
  int thumbHeight = std::max(1, static_cast<int>(
      (static_cast<int64_t>(visibleRows) * barHeight + totalRows - 1) /
      totalRows));
  if (thumbHeight > barHeight) thumbHeight = barHeight;
  int thumbTravel = barHeight - thumbHeight;
  int scroll = std::clamp(scrollRow, 0, maxScroll);
  int thumbTop = top;
  if (thumbTravel > 0) {
    thumbTop += static_cast<int>(
        (static_cast<int64_t>(scroll) * thumbTravel + maxScroll / 2) /
        maxScroll);
  }
  for (int col = 0; col < width; ++col) {
    for (int y = 0; y < barHeight; ++y) {
      screen.writeChar(x + col, top + y, L'\u2591', trackStyle);
    }
    for (int y = 0; y < thumbHeight; ++y) {
      screen.writeChar(x + col, thumbTop + y, L'\u2588', thumbStyle);
    }
  }
}

static void assignThumbnailFromAscii(const AsciiArt& art, Thumbnail& out) {
  out.ok = true;
  out.width = art.width;
  out.height = art.height;
  out.cells.resize(art.cells.size());
  for (size_t i = 0; i < art.cells.size(); ++i) {
    const auto& cell = art.cells[i];
    auto& dst = out.cells[i];
    dst.ch = cell.ch;
    dst.fg = cell.fg;
    dst.bg = cell.bg;
    dst.hasBg = cell.hasBg;
  }
}

static bool renderImageThumbnail(const std::filesystem::path& file,
                                 int maxWidth, int maxHeight, Thumbnail& out,
                                 std::string* error) {
  AsciiArt art;
  if (!renderAsciiArt(file, maxWidth, maxHeight, art, error)) return false;
  assignThumbnailFromAscii(art, out);
  return true;
}

static void computeVideoThumbTarget(int thumbWidth, int thumbHeight, int& outW,
                                    int& outH) {
  outW = std::max(2, thumbWidth * 2);
  outH = std::max(4, thumbHeight * 4);
  if (outW & 1) ++outW;
  if (outH & 1) ++outH;
}

static bool renderVideoThumbnail(const std::filesystem::path& file,
                                 int maxWidth, int maxHeight, Thumbnail& out,
                                 std::string* error) {
  if (maxWidth <= 0 || maxHeight <= 0) return false;
  VideoDecoder decoder;
  std::string initError;
  if (!decoder.init(file, &initError, true, false)) {
    if (!decoder.init(file, &initError, false, false)) {
      if (error && !initError.empty()) *error = initError;
      return false;
    }
  }

  int targetW = 0;
  int targetH = 0;
  computeVideoThumbTarget(maxWidth, maxHeight, targetW, targetH);
  decoder.setTargetSize(targetW, targetH, nullptr);

  VideoFrame frame;
  VideoReadInfo info;
  for (int i = 0; i < 20; ++i) {
    if (!decoder.readFrame(frame, &info, true)) {
      if (decoder.atEnd()) break;
      continue;
    }

    AsciiArt art;
    bool ok = false;
    if ((frame.format == VideoPixelFormat::NV12 ||
         frame.format == VideoPixelFormat::P010) &&
        !frame.yuv.empty()) {
      YuvFormat yuvFormat = (frame.format == VideoPixelFormat::P010)
                                ? YuvFormat::P010
                                : YuvFormat::NV12;
      ok = renderAsciiArtFromYuv(
          frame.yuv.data(), frame.width, frame.height, frame.stride,
          frame.planeHeight, yuvFormat, frame.fullRange, frame.yuvMatrix,
          frame.yuvTransfer, maxWidth, maxHeight, art);
    } else if (!frame.rgba.empty()) {
      ok = renderAsciiArtFromRgba(frame.rgba.data(), frame.width, frame.height,
                                  maxWidth, maxHeight, art, true);
    }
    if (ok) {
      assignThumbnailFromAscii(art, out);
      return true;
    }
  }
  if (error && !initError.empty()) *error = initError;
  return false;
}

static bool renderAudioThumbnail(const std::filesystem::path& file,
                                 int trackIndex, int maxWidth, int maxHeight,
                                 Thumbnail& out, std::string* error) {
  PlaybackMediaDisplayRequest request;
  request.file = file;
  request.trackIndex = trackIndex;
  request.isVideo = false;

  AsciiArt art;
  std::string artworkError;
  if (!resolvePlaybackMediaArtworkAscii(
          request, MediaArtworkSidecarPolicy::FileSpecificOnly, maxWidth,
          maxHeight, &art, &artworkError)) {
    if (error) {
      *error = artworkError;
    }
    return false;
  }

  assignThumbnailFromAscii(art, out);
  return true;
}

static bool executeThumbnailJob(const ThumbJob& job, Thumbnail& thumb,
                                std::string* error) {
  try {
    if (job.isImage) {
      return renderImageThumbnail(job.path, job.width, job.height, thumb, error);
    }
    if (job.isVideo) {
      return renderVideoThumbnail(job.path, job.width, job.height, thumb, error);
    }
    if (job.isAudio) {
      return renderAudioThumbnail(job.path, job.trackIndex, job.width,
                                  job.height, thumb, error);
    }
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
    return false;
  } catch (...) {
    if (error) {
      *error = "Thumbnail preview threw an unknown exception.";
    }
    return false;
  }

  return false;
}

#ifdef _WIN32
static bool executeThumbnailJobGuarded(const ThumbJob& job, Thumbnail& thumb,
                                       std::string* error) {
  // Preview decode must never be able to take down the whole TUI.
  __try {
    return executeThumbnailJob(job, thumb, error);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (error) {
      *error = "Thumbnail preview decoder faulted.";
    }
    return false;
  }
}
#else
static bool executeThumbnailJobGuarded(const ThumbJob& job, Thumbnail& thumb,
                                       std::string* error) {
  return executeThumbnailJob(job, thumb, error);
}
#endif

static void thumbWorkerLoop() {
  ThumbCacheState& cache = thumbCache();
  for (;;) {
    ThumbJob job;
    {
      std::unique_lock<std::mutex> lock(cache.mutex);
      cache.cv.wait(lock, [&]() { return !cache.queue.empty(); });
      job = std::move(cache.queue.front());
      cache.queue.pop_front();
    }

    Thumbnail thumb;
    std::string error;
    const bool ok = executeThumbnailJobGuarded(job, thumb, &error);

    {
      std::lock_guard<std::mutex> lock(cache.mutex);
      if (job.generation != cache.generation) {
        continue;
      }
      auto it = cache.entries.find(job.key);
      if (it != cache.entries.end()) {
        if (ok) {
          it->second.state = ThumbState::Ready;
          it->second.thumb = std::make_shared<Thumbnail>(std::move(thumb));
        } else {
          it->second.state = ThumbState::Failed;
          it->second.thumb.reset();
        }
      }
    }
    if (cache.thumbnailReadyEvent) {
      SetEvent(cache.thumbnailReadyEvent);
    }
  }
}

static void startThumbWorkerLocked(ThumbCacheState& cache) {
  if (cache.workerStarted) return;
  cache.workerStarted = true;
  std::thread(thumbWorkerLoop).detach();
}

HANDLE browserThumbnailWakeHandle() {
  return thumbCache().thumbnailReadyEvent;
}

bool consumeBrowserThumbnailWake() {
  ThumbCacheState& cache = thumbCache();
  if (!cache.thumbnailReadyEvent) return false;
  DWORD state = WaitForSingleObject(cache.thumbnailReadyEvent, 0);
  if (state != WAIT_OBJECT_0) return false;
  ResetEvent(cache.thumbnailReadyEvent);
  return true;
}

GridLayout buildLayout(const BrowserState& state, int width, int listHeight) {
  GridLayout layout;
  layout.names.reserve(state.entries.size());
  bool hasSectionHeaders = std::any_of(state.entries.begin(), state.entries.end(),
                                       [](const FileEntry& entry) {
                                         return entry.isSectionHeader;
                                       });
  int maxName = 0;
  for (const auto& e : state.entries) {
    std::string name = e.name;
    if (e.isDir && name != "..") name += "/";
    layout.names.push_back(name);
    maxName = std::max(maxName, utf8DisplayWidth(name) + 2);
  }

  constexpr int kThumbMinWidth = 20;
  constexpr int kThumbTargetWidth = 32;
  constexpr int kThumbMinHeight = 8;
  constexpr int kThumbMaxHeight = 24;
  constexpr int kThumbLabelRows = 1;
  constexpr int kPreviewMinWidth = 24;
  constexpr int kPreviewMinHeight = 8;
  constexpr int kPreviewGap = 2;
  constexpr int kListMinWidth = 20;
  constexpr int kScrollBarWidth = 2;

  layout.showThumbs =
      state.viewMode == BrowserState::ViewMode::Thumbnails &&
      (listHeight >= kThumbMinHeight + kThumbLabelRows &&
       width >= kThumbMinWidth + 2);
  layout.listWidth = width;
  layout.showPreview = false;
  layout.previewX = 0;
  layout.previewWidth = 0;
  layout.previewHeight = 0;
  if (layout.showThumbs) {
    layout.thumbHeight =
        std::clamp(listHeight - kThumbLabelRows, kThumbMinHeight,
                   kThumbMaxHeight);
    layout.cellHeight = layout.thumbHeight + kThumbLabelRows;
    int minColWidth = std::max(8, maxName + 2);
    int maxThumbWidth = std::max(1, width - 2);
    int targetThumbWidth = std::min(kThumbTargetWidth, maxThumbWidth);
    if (targetThumbWidth < kThumbMinWidth &&
        maxThumbWidth >= kThumbMinWidth) {
      targetThumbWidth = kThumbMinWidth;
    }
    int desiredColWidth = std::max(minColWidth, targetThumbWidth + 2);
    layout.colWidth = std::min(width, desiredColWidth);
    layout.colWidth = std::max(layout.colWidth, 6);
    layout.thumbWidth = std::max(1, layout.colWidth - 2);
  } else {
    int listWidth = width;
    if (state.viewMode == BrowserState::ViewMode::ListPreview &&
        width >= kListMinWidth + kPreviewMinWidth + kPreviewGap &&
        listHeight >= kPreviewMinHeight) {
      int baseListWidth = std::max(kListMinWidth, maxName + 3);
      int maxListWidth = width - kPreviewMinWidth - kPreviewGap;
      if (maxListWidth >= kListMinWidth) {
        int listCandidate = std::min(baseListWidth, maxListWidth);
        int previewWidth = width - listCandidate - kPreviewGap;
        if (previewWidth >= kPreviewMinWidth) {
          layout.showPreview = true;
          layout.previewWidth = previewWidth;
          layout.previewHeight = std::max(kPreviewMinHeight, listHeight);
          layout.previewX = listCandidate + kPreviewGap;
          listWidth = listCandidate;
        }
      }
    }
    layout.listWidth = std::max(1, listWidth);
    layout.cellHeight = 1;
    layout.thumbWidth = 0;
    layout.thumbHeight = 0;
    layout.colWidth =
        std::min(layout.listWidth, std::max(8, maxName + 3));
  }

  int safeRows = std::max(1, listHeight / std::max(1, layout.cellHeight));
  int maxCols = std::max(1, layout.listWidth / std::max(1, layout.colWidth));
  int cols = std::max(
      1,
      std::min(maxCols, static_cast<int>((state.entries.size() + safeRows - 1) /
                                         safeRows)));
  int totalRows = browserGridTotalRowsForEntryCount(
      state.viewMode, static_cast<int>(state.entries.size()), safeRows, cols);
  int visibleRows = std::min(safeRows, totalRows);

  if (hasSectionHeaders) {
    layout.showThumbs = false;
    layout.showPreview = false;
    layout.previewX = 0;
    layout.previewWidth = 0;
    layout.previewHeight = 0;
    layout.listWidth = std::max(1, width);
    layout.cellHeight = 1;
    layout.thumbWidth = 0;
    layout.thumbHeight = 0;
    layout.colWidth = layout.listWidth;
    layout.cols = 1;
    layout.totalRows = static_cast<int>(state.entries.size());
    layout.rowsVisible = std::min(std::max(1, safeRows), layout.totalRows);
    if (layout.rowsVisible <= 0) layout.rowsVisible = 1;
    layout.showScrollBar = (layout.totalRows > layout.rowsVisible);
    layout.scrollBarWidth = 0;
    layout.scrollBarX = -1;
    if (layout.showScrollBar) {
      int barWidth = std::min(kScrollBarWidth, std::max(1, width));
      layout.scrollBarWidth = barWidth;
      layout.scrollBarX = std::max(0, width - barWidth);
    }
    return layout;
  }

  layout.rowsVisible = visibleRows;
  layout.totalRows = totalRows;
  layout.cols = cols;
  layout.showScrollBar = (totalRows > visibleRows);
  layout.scrollBarWidth = 0;
  layout.scrollBarX = -1;
  if (layout.showScrollBar) {
    int barWidth = std::min(kScrollBarWidth, std::max(1, width));
    int listUsedWidth = std::max(1, layout.colWidth) * cols;
    layout.scrollBarWidth = barWidth;
    if (layout.showPreview) {
      layout.scrollBarX = std::max(0, layout.previewX - barWidth);
    } else if (listUsedWidth + barWidth <= width) {
      layout.scrollBarX = listUsedWidth;
    } else {
      layout.scrollBarX = std::max(0, width - barWidth);
    }
    if (layout.scrollBarX + barWidth > width) {
      layout.scrollBarX = std::max(0, width - barWidth);
    }
  }
  return layout;
}

void drawBrowserEntries(ConsoleScreen& screen, const BrowserState& browser,
                        const GridLayout& layout, int listTop, int listHeight,
                        const Style& baseStyle, const Style& normalStyle,
                        const Style& dirStyle, const Style& highlightStyle,
                        const Style& dimStyle,
                        bool (*isImage)(const std::filesystem::path&),
                        bool (*isVideo)(const std::filesystem::path&),
                        bool (*isAudio)(const std::filesystem::path&)) {
  ThumbCacheState& cache = thumbCache();
  if (browser.entries.empty()) {
    screen.writeText(2, listTop, "(no supported files)", dimStyle);
    return;
  }

  int desiredThumbW = 0;
  int desiredThumbH = 0;
  if (layout.showThumbs) {
    desiredThumbW = layout.thumbWidth;
    desiredThumbH = layout.thumbHeight;
  } else if (layout.showPreview) {
    desiredThumbW = layout.previewWidth;
    desiredThumbH = layout.previewHeight;
  }
  if (desiredThumbW > 0 && desiredThumbH > 0) {
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.thumbW != desiredThumbW || cache.thumbH != desiredThumbH) {
      cache.thumbW = desiredThumbW;
      cache.thumbH = desiredThumbH;
      cache.generation++;
      cache.entries.clear();
      cache.queue.clear();
    }
  }

  auto entryPrefix = [&](const FileEntry& entry) -> std::string {
    if (entry.isDir) return "\xF0\x9F\x93\x81 ";
    if (entry.trackIndex >= 0) return "\xE2\x99\xAA ";
    if (isAudio && isAudio(entry.path)) return "\xE2\x99\xAA ";
    if (isVideo && isVideo(entry.path)) return "\xE2\x96\xB6 ";
    if (isImage && isImage(entry.path)) return "\xE2\x96\xA1 ";
    return "\xC2\xB7 ";
  };

  constexpr int kThumbJobsPerFrame = 4;
  constexpr size_t kMaxThumbQueue = 64;
  int enqueueBudget =
      layout.showThumbs ? kThumbJobsPerFrame : (layout.showPreview ? 1 : 0);

  struct ThumbLookup {
    std::shared_ptr<Thumbnail> thumb;
    bool pending = false;
  };

  auto fetchThumb = [&](const FileEntry& entry, int width, int height,
                        bool wantImage, bool wantVideo,
                        bool wantAudio) -> ThumbLookup {
    ThumbLookup result;
    if (!wantImage && !wantVideo && !wantAudio) return result;
    if (width <= 0 || height <= 0) return result;
    std::string key = thumbnailCacheKey(entry);
    {
      std::lock_guard<std::mutex> lock(cache.mutex);
      auto it = cache.entries.find(key);
      if (it != cache.entries.end()) {
        if (it->second.state == ThumbState::Ready) {
          result.thumb = it->second.thumb;
        } else if (it->second.state == ThumbState::Pending) {
          result.pending = true;
        }
        return result;
      }
      if (enqueueBudget <= 0) return result;
      if (cache.queue.size() >= kMaxThumbQueue) return result;
      ThumbEntry entryState;
      entryState.state = ThumbState::Pending;
      cache.entries.emplace(key, entryState);
      ThumbJob job;
      job.key = key;
      job.path = entry.path;
      job.isImage = wantImage;
      job.isVideo = wantVideo;
      job.isAudio = wantAudio;
      job.trackIndex = entry.trackIndex;
      job.width = width;
      job.height = height;
      job.generation = cache.generation;
      cache.queue.push_back(std::move(job));
      startThumbWorkerLocked(cache);
      cache.cv.notify_one();
      enqueueBudget--;
      result.pending = true;
    }
    return result;
  };

  if (!layout.showThumbs) {
    for (int r = 0; r < layout.rowsVisible; ++r) {
      int y = listTop + r;
      int logicalRow = r + browser.scrollRow;
      if (logicalRow >= layout.totalRows) continue;
      for (int c = 0; c < layout.cols; ++c) {
        int idx = browserGridEntryIndex(layout, browser.viewMode, logicalRow, c,
                                        static_cast<int>(browser.entries.size()));
        if (idx < 0 || idx >= static_cast<int>(browser.entries.size())) continue;
        const auto& entry = browser.entries[static_cast<size_t>(idx)];
        bool isSelected = (idx == browser.selected);

        if (entry.isSectionHeader) {
          std::string cell = fitName("[" + entry.name + "]", layout.colWidth);
          int cellWidth = utf8DisplayWidth(cell);
          if (cellWidth < layout.colWidth) {
            cell.append(static_cast<size_t>(layout.colWidth - cellWidth), ' ');
          } else if (cellWidth > layout.colWidth) {
            cell = utf8TakeDisplayWidth(cell, layout.colWidth);
          }
          Style headerStyle = dimStyle;
          if (isSelected) {
            headerStyle = dimStyle;
          }
          screen.writeText(c * layout.colWidth, y, cell, headerStyle);
          continue;
        }

        std::string cell = fitName(entryPrefix(entry) +
                                       layout.names[static_cast<size_t>(idx)],
                                   layout.colWidth);
        int cellWidth = utf8DisplayWidth(cell);
        if (cellWidth < layout.colWidth) {
          cell.append(static_cast<size_t>(layout.colWidth - cellWidth), ' ');
        } else if (cellWidth > layout.colWidth) {
          cell = utf8TakeDisplayWidth(cell, layout.colWidth);
        }
        Style attr =
            isSelected ? highlightStyle : (entry.isDir ? dirStyle : normalStyle);
        screen.writeText(c * layout.colWidth, y, cell, attr);
      }
    }
    if (layout.showPreview && !browser.entries.empty()) {
      int idx = std::clamp(
          browser.selected, 0,
          static_cast<int>(browser.entries.size()) - 1);
      const auto& entry = browser.entries[static_cast<size_t>(idx)];
      bool img = !entry.isDir && isImage && isImage(entry.path);
      bool vid = !entry.isDir && isVideo && isVideo(entry.path);
      bool aud = !entry.isDir && isAudio && isAudio(entry.path);
      int previewW = std::max(1, layout.previewWidth);
      int previewH = std::max(1, layout.previewHeight);
      int previewX = layout.previewX;
      int previewY = listTop + std::max(0, (listHeight - previewH) / 2);

      ThumbLookup lookup = fetchThumb(entry, previewW, previewH, img, vid, aud);
      const Thumbnail* thumb = lookup.thumb.get();
      if (thumb && thumb->ok && thumb->width > 0 && thumb->height > 0) {
        int artW = std::min(thumb->width, previewW);
        int artH = std::min(thumb->height, previewH);
        int artX = previewX + std::max(0, (previewW - artW) / 2);
        int artY = previewY + std::max(0, (previewH - artH) / 2);
        for (int y = 0; y < artH; ++y) {
          for (int x = 0; x < artW; ++x) {
            const auto& cell =
                thumb->cells[static_cast<size_t>(y * thumb->width + x)];
            Style cellStyle{cell.fg, cell.hasBg ? cell.bg : baseStyle.bg};
            screen.writeChar(artX + x, artY + y, cell.ch, cellStyle);
          }
        }
      } else {
        std::string placeholder;
        if (entry.isDir) {
          placeholder = "\xF0\x9F\x93\x81";
        } else if (img) {
          placeholder = "\xE2\x96\xA1";
        } else if (vid) {
          placeholder = "\xE2\x96\xB6";
        } else if (aud) {
          placeholder = "\xE2\x99\xAA";
        } else {
          placeholder = "\xC2\xB7";
        }
        if ((img || vid || aud) && lookup.pending) {
          placeholder = "...";
        }
        placeholder = fitLine(placeholder, previewW);
        drawCenteredText(screen, previewX, previewY, previewW, previewH,
                         placeholder, dimStyle);
      }
    }
    if (layout.showScrollBar) {
      drawScrollBar(screen, layout.scrollBarX, layout.scrollBarWidth, listTop,
                    listHeight, browser.scrollRow, layout.totalRows,
                    layout.rowsVisible, dimStyle, normalStyle);
    }
    return;
  }

  for (int r = 0; r < layout.rowsVisible; ++r) {
    int logicalRow = r + browser.scrollRow;
    if (logicalRow >= layout.totalRows) continue;
    int cellTop = listTop + r * layout.cellHeight;
    for (int c = 0; c < layout.cols; ++c) {
      int idx = logicalRow * layout.cols + c;
      if (idx >= static_cast<int>(browser.entries.size())) continue;
      const auto& entry = browser.entries[static_cast<size_t>(idx)];
      bool isSelected = (idx == browser.selected);
      int cellLeft = c * layout.colWidth;
      int thumbX = cellLeft + 1;
      int thumbY = cellTop;
      int thumbW = std::max(1, layout.thumbWidth);
      int thumbH = std::max(1, layout.thumbHeight);
      bool img = !entry.isDir && isImage && isImage(entry.path);
      bool vid = !entry.isDir && isVideo && isVideo(entry.path);
      bool aud = !entry.isDir && isAudio && isAudio(entry.path);

      ThumbLookup lookup = fetchThumb(entry, thumbW, thumbH, img, vid, aud);
      const Thumbnail* thumb = lookup.thumb.get();
      if (thumb && thumb->ok && thumb->width > 0 && thumb->height > 0) {
        int artW = std::min(thumb->width, thumbW);
        int artH = std::min(thumb->height, thumbH);
        int artX = thumbX + std::max(0, (thumbW - artW) / 2);
        int artY = thumbY + std::max(0, (thumbH - artH) / 2);
        for (int y = 0; y < artH; ++y) {
          for (int x = 0; x < artW; ++x) {
            const auto& cell =
                thumb->cells[static_cast<size_t>(y * thumb->width + x)];
            Style cellStyle{cell.fg, cell.hasBg ? cell.bg : baseStyle.bg};
            screen.writeChar(artX + x, artY + y, cell.ch, cellStyle);
          }
        }
      } else {
        std::string placeholder;
        if (entry.isDir) {
          placeholder = "\xF0\x9F\x93\x81";
        } else if (img) {
          placeholder = "\xE2\x96\xA1";
        } else if (vid) {
          placeholder = "\xE2\x96\xB6";
        } else if (aud) {
          placeholder = "\xE2\x99\xAA";
        } else {
          placeholder = "\xC2\xB7";
        }
        if ((img || vid || aud) && lookup.pending) {
          placeholder = "...";
        }
        placeholder = fitLine(placeholder, thumbW);
        drawCenteredText(screen, thumbX, thumbY, thumbW, thumbH, placeholder,
                         dimStyle);
      }

      int labelY = cellTop + layout.thumbHeight;
      if (labelY < listTop + listHeight) {
        std::string label = fitName(entryPrefix(entry) +
                                        layout.names[static_cast<size_t>(idx)],
                                    layout.colWidth);
        int labelWidth = utf8DisplayWidth(label);
        int labelX =
            cellLeft + std::max(0, (layout.colWidth - labelWidth) / 2);
        Style labelStyle =
            isSelected ? highlightStyle : (entry.isDir ? dirStyle : normalStyle);
        if (isSelected) {
          screen.writeRun(cellLeft, labelY, layout.colWidth, L' ', labelStyle);
        }
        screen.writeText(labelX, labelY, label, labelStyle);
      }
    }
  }
  if (layout.showScrollBar) {
    drawScrollBar(screen, layout.scrollBarX, layout.scrollBarWidth, listTop,
                  listHeight, browser.scrollRow, layout.totalRows,
                  layout.rowsVisible, dimStyle, normalStyle);
  }
}

BreadcrumbLine buildBreadcrumbLine(const std::filesystem::path& dir, int width) {
  BreadcrumbLine line;
  const std::string prefix = "  Path: ";
  const int prefixLen = utf8DisplayWidth(prefix);
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
  items.push_back(Item{"This PC", {}});
  if (!root.empty()) {
    std::string rootLabel = toUtf8String(dir.root_name());
    if (rootLabel.empty()) rootLabel = toUtf8String(root);
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
    items.push_back(Item{toUtf8String(part), cur});
  }
  if (items.empty()) {
    items.push_back(Item{toUtf8String(dir), dir});
  }

  const std::string sep = " > ";
  const int sepLen = utf8DisplayWidth(sep);
  const std::string ellipsis = "... > ";
  const int ellipsisLen = utf8DisplayWidth(ellipsis);

  const int avail = width - prefixLen;
  std::vector<Item> visibleRev;
  int used = 0;
  int index = static_cast<int>(items.size()) - 1;
  for (; index >= 0; --index) {
    int labelLen = utf8DisplayWidth(items[index].label);
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
    used = utf8DisplayWidth(last.label);
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
    int labelLen = utf8DisplayWidth(item.label);
    if (labelLen > remaining) {
      item.label = fitLine(item.label, remaining);
      labelLen = utf8DisplayWidth(item.label);
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
    std::string fallback = fitLine(toUtf8String(dir), width - prefixLen);
    line.text = prefix + fallback;
  }
  if (line.crumbs.empty()) {
    int total = utf8DisplayWidth(line.text);
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
  originalOutputCp_ = GetConsoleOutputCP();
  useUtf8Output_ = false;
  DWORD mode = originalMode_;
  mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT |
          DISABLE_NEWLINE_AUTO_RETURN;
  mode &= ~ENABLE_WRAP_AT_EOL_OUTPUT;
  SetConsoleMode(out_, mode);
  CONSOLE_CURSOR_INFO cursor{};
  if (GetConsoleCursorInfo(out_, &cursor)) {
    originalCursor_ = cursor;
    cursor.bVisible = FALSE;
    SetConsoleCursorInfo(out_, &cursor);
  }
  if (useUtf8Output_) {
    SetConsoleOutputCP(CP_UTF8);
  }
  updateCellPixelSize();
  std::wstring seq = L"\x1b[?1049h\x1b[H\x1b[?25l";
  if (!writeOutput(seq)) {
    reportWriteError(L"init");
    altScreen_ = false;
    hasPrev_ = false;
    updateSize();
    return true;
  }
  altScreen_ = true;
  hasPrev_ = false;
  syncScreenBufferToWindow();
  updateSize();
  return true;
}

void ConsoleScreen::restore() {
  if (out_ != INVALID_HANDLE_VALUE) {
    if (altScreen_) {
      std::wstring seq = L"\x1b[0m\x1b[?25h\x1b[?1049l";
      if (!writeOutput(seq)) {
        reportWriteError(L"restore");
      }
      altScreen_ = false;
    }
    if (useUtf8Output_ && originalOutputCp_ != 0) {
      SetConsoleOutputCP(originalOutputCp_);
    }
    SetConsoleMode(out_, originalMode_);
    if (originalCursor_.dwSize != 0) {
      SetConsoleCursorInfo(out_, &originalCursor_);
    }
  }
}

void ConsoleScreen::updateSize() {
  if (virtualSize_) {
    return;
  }
  if (altScreen_) {
    syncScreenBufferToWindow();
  }
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(out_, &info)) {
    applySize(80, 25);
    return;
  }
  applySize(info.srWindow.Right - info.srWindow.Left + 1,
            info.srWindow.Bottom - info.srWindow.Top + 1);
}

void ConsoleScreen::syncScreenBufferToWindow() {
  if (out_ == INVALID_HANDLE_VALUE) return;

  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(out_, &info)) return;

  const SHORT windowWidth =
      static_cast<SHORT>(info.srWindow.Right - info.srWindow.Left + 1);
  const SHORT windowHeight =
      static_cast<SHORT>(info.srWindow.Bottom - info.srWindow.Top + 1);
  if (windowWidth <= 0 || windowHeight <= 0) return;

  SMALL_RECT window{0, 0, static_cast<SHORT>(windowWidth - 1),
                    static_cast<SHORT>(windowHeight - 1)};
  const bool originMismatch =
      info.srWindow.Left != window.Left || info.srWindow.Top != window.Top ||
      info.srWindow.Right != window.Right ||
      info.srWindow.Bottom != window.Bottom;
  if (originMismatch) {
    SetConsoleWindowInfo(out_, TRUE, &window);
    GetConsoleScreenBufferInfo(out_, &info);
  }

  COORD bufferSize{windowWidth, windowHeight};
  if (info.dwSize.X != bufferSize.X || info.dwSize.Y != bufferSize.Y) {
    SetConsoleScreenBufferSize(out_, bufferSize);
    SetConsoleWindowInfo(out_, TRUE, &window);
  }
}

void ConsoleScreen::updateCellPixelSize() {
  HDC memDC = CreateCompatibleDC(nullptr);
  if (!memDC) return;

  const RadioifyTerminalFontMetrics metrics =
      measureRadioifyTerminalFontMetrics(memDC, USER_DEFAULT_SCREEN_DPI);
  DeleteDC(memDC);
  cellPixelWidth_ = std::max(1, metrics.cellWidth);
  cellPixelHeight_ = std::max(1, metrics.cellHeight);
}

void ConsoleScreen::setVirtualSize(int width, int height) {
  virtualSize_ = true;
  applySize(width, height);
}

void ConsoleScreen::clearVirtualSize() {
  virtualSize_ = false;
  updateSize();
}

void ConsoleScreen::applySize(int width, int height) {
  int oldW = width_;
  int oldH = height_;
  width_ = width;
  height_ = height;
  if (width_ < 1) width_ = 1;
  if (height_ < 1) height_ = 1;
  size_t needed = static_cast<size_t>(width_ * height_);
  bool dimsChanged = (width_ != oldW || height_ != oldH);
  if (dimsChanged || buffer_.size() != needed) {
    buffer_.assign(needed, {});
  }
  if (dimsChanged || prevBuffer_.size() != needed) {
    prevBuffer_.assign(needed, {});
    hasPrev_ = false;
  }
  if (dimsChanged) {
    outputFailed_ = false;
    outputErrorReported_ = false;
    clearOnNextFullRedraw_ = true;
  }
}

int ConsoleScreen::width() const { return width_; }
int ConsoleScreen::height() const { return height_; }
int ConsoleScreen::cellPixelWidth() const { return cellPixelWidth_; }
int ConsoleScreen::cellPixelHeight() const { return cellPixelHeight_; }

void ConsoleScreen::setCellSpace(Cell& cell, const Style& style) {
  cell.glyph[0] = L' ';
  cell.glyph[1] = L'\0';
  cell.glyphLen = 1;
  cell.cellWidth = 1;
  cell.continuation = false;
  cell.fg = style.fg;
  cell.bg = style.bg;
}

void ConsoleScreen::setCellGlyph(Cell& cell, const wchar_t* glyph,
                                 uint8_t glyphLen, uint8_t cellWidth,
                                 bool continuation, const Style& style) {
  cell.glyph[0] = L' ';
  cell.glyph[1] = L'\0';
  cell.glyphLen = glyphLen;
  cell.cellWidth = cellWidth;
  cell.continuation = continuation;
  if (glyph && glyphLen > 0) {
    cell.glyph[0] = glyph[0];
    if (glyphLen > 1) {
      cell.glyph[1] = glyph[1];
    }
  }
  cell.fg = style.fg;
  cell.bg = style.bg;
}

bool ConsoleScreen::sameCell(const Cell& a, const Cell& b) {
  return a.glyph[0] == b.glyph[0] && a.glyph[1] == b.glyph[1] &&
         a.glyphLen == b.glyphLen && a.cellWidth == b.cellWidth &&
         a.continuation == b.continuation && sameColor(a.fg, b.fg) &&
         sameColor(a.bg, b.bg);
}

void ConsoleScreen::appendCellGlyph(std::wstring& out, const Cell& cell) {
  if (cell.continuation) return;
  if (cell.glyphLen == 0) {
    out.push_back(L' ');
    return;
  }
  out.push_back(cell.glyph[0] ? cell.glyph[0] : L' ');
  if (cell.glyphLen > 1 && cell.glyph[1] != L'\0') {
    out.push_back(cell.glyph[1]);
  }
}

void ConsoleScreen::clear(const Style& style) {
  Cell cell{};
  setCellSpace(cell, style);
  std::fill(buffer_.begin(), buffer_.end(), cell);
}

void ConsoleScreen::writeText(int x, int y, const std::string& text, const Style& style) {
  if (y < 0 || y >= height_) return;
  if (x >= width_) return;
  int start = std::max(0, x);
  std::string clipped =
      (x < 0) ? utf8SliceDisplayWidth(text, -x, width_ - start) : text;
  int col = start;
  size_t offset = 0;
  char32_t codepoint = 0;
  size_t glyphStart = 0;
  size_t glyphEnd = 0;
  while (col < width_ &&
         decodeUtf8Codepoint(clipped, &offset, &codepoint, &glyphStart,
                             &glyphEnd)) {
    int glyphWidth = unicodeDisplayWidth(codepoint);
    if (glyphWidth <= 0) continue;
    if (glyphWidth > width_ - col) break;
    wchar_t glyph[2]{L' ', L'\0'};
    uint8_t glyphLen = encodeUtf16(codepoint, glyph);
    int pos = y * width_ + col;
    setCellGlyph(buffer_[pos], glyph, glyphLen,
                 static_cast<uint8_t>(glyphWidth), false, style);
    for (int extra = 1; extra < glyphWidth; ++extra) {
      setCellGlyph(buffer_[pos + extra], nullptr, 0, 0, true, style);
    }
    col += glyphWidth;
  }
}

void ConsoleScreen::writeRun(int x, int y, int len, wchar_t ch, const Style& style) {
  if (y < 0 || y >= height_) return;
  if (x >= width_) return;
  int start = std::max(0, x);
  int maxLen = std::min(len, width_ - start);
  int pos = y * width_ + start;
  for (int i = 0; i < maxLen; ++i) {
    wchar_t glyph[2]{ch, L'\0'};
    setCellGlyph(buffer_[pos + i], glyph, 1, 1, false, style);
  }
}

void ConsoleScreen::writeChar(int x, int y, wchar_t ch, const Style& style) {
  if (y < 0 || y >= height_) return;
  if (x < 0 || x >= width_) return;
  int glyphWidth = std::max(1, unicodeDisplayWidth(static_cast<char32_t>(ch)));
  if (glyphWidth > width_ - x) return;
  int pos = y * width_ + x;
  wchar_t glyph[2]{ch, L'\0'};
  setCellGlyph(buffer_[pos], glyph, 1, static_cast<uint8_t>(glyphWidth), false,
               style);
  for (int extra = 1; extra < glyphWidth; ++extra) {
    setCellGlyph(buffer_[pos + extra], nullptr, 0, 0, true, style);
  }
}

void ConsoleScreen::setFastOutput(bool enabled) {
  if (fastOutput_ == enabled) return;
  fastOutput_ = enabled;
  hasPrev_ = false;
  outputFailed_ = false;
  outputErrorReported_ = false;
}

void ConsoleScreen::setAlwaysFullRedraw(bool enabled) {
  if (alwaysFullRedraw_ == enabled) return;
  alwaysFullRedraw_ = enabled;
  hasPrev_ = false;
}

bool ConsoleScreen::fastOutput() const { return fastOutput_; }

bool ConsoleScreen::snapshot(std::vector<ScreenCell>& outCells, int& outWidth,
                             int& outHeight) const {
  outWidth = width_;
  outHeight = height_;
  if (width_ <= 0 || height_ <= 0 || buffer_.empty()) {
    outCells.clear();
    return false;
  }
  outCells.resize(buffer_.size());
  for (size_t i = 0; i < buffer_.size(); ++i) {
    const Cell& src = buffer_[i];
    ScreenCell& dst = outCells[i];
    dst.ch = src.continuation
                 ? L' '
                 : (src.glyphLen == 1 ? src.glyph[0] : L'\uFFFD');
    dst.cellWidth = src.continuation ? 0 : std::max<uint8_t>(1, src.cellWidth);
    dst.continuation = src.continuation;
    dst.fg = src.fg;
    dst.bg = src.bg;
  }
  return true;
}

bool ConsoleScreen::writeOutput(const std::wstring& text) {
  if (out_ == INVALID_HANDLE_VALUE) return false;
  if (text.empty()) return true;
  if (outputFailed_) return false;
  if (useUtf8Output_) {
    if (!wideToUtf8(text, drawUtf8Buffer_)) {
      outputError_ = GetLastError();
      outputFailed_ = true;
      return false;
    }
    DWORD written = 0;
    if (!WriteFile(out_, drawUtf8Buffer_.data(),
                   static_cast<DWORD>(drawUtf8Buffer_.size()), &written,
                   nullptr)) {
      outputError_ = GetLastError();
      outputFailed_ = true;
      return false;
    }
    if (written != drawUtf8Buffer_.size()) {
      outputError_ = ERROR_WRITE_FAULT;
      outputFailed_ = true;
      return false;
    }
    return true;
  }
  DWORD written = 0;
  if (!WriteConsoleW(out_, text.c_str(), static_cast<DWORD>(text.size()),
                     &written, nullptr)) {
    outputError_ = GetLastError();
    outputFailed_ = true;
    return false;
  }
  if (written != text.size()) {
    outputError_ = ERROR_WRITE_FAULT;
    outputFailed_ = true;
    return false;
  }
  return true;
}

void ConsoleScreen::reportWriteError(const wchar_t* context) {
  if (!outputFailed_ || outputErrorReported_ || out_ == INVALID_HANDLE_VALUE) {
    return;
  }
  outputErrorReported_ = true;
  std::wstring msg;
  msg.append(L"\x1b[0m\r\n[console] output failed in ");
  msg.append(context ? context : L"draw");
  msg.append(L" (err=");
  appendNumber(msg, static_cast<int>(outputError_));
  msg.append(L")\r\n");
  DWORD written = 0;
  WriteConsoleW(out_, msg.c_str(), static_cast<DWORD>(msg.size()), &written,
                nullptr);
}

void ConsoleScreen::drawFast() {
  if (out_ == INVALID_HANDLE_VALUE) return;
  if (outputFailed_) {
    reportWriteError(L"drawFast");
    return;
  }
  size_t needed = static_cast<size_t>(width_ * height_);
  if (buffer_.size() < needed) {
    updateSize();
    if (buffer_.size() < needed) return;
  }
  if (fastBuffer_.size() != needed) {
    fastBuffer_.resize(needed);
  }
  for (size_t i = 0; i < needed; ++i) {
    const Cell& cell = buffer_[i];
    CHAR_INFO& outCell = fastBuffer_[i];
    outCell.Char.UnicodeChar =
        cell.continuation ? L' ' : (cell.glyphLen == 1 ? cell.glyph[0] : L' ');
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
  if (outputFailed_) {
    reportWriteError(L"draw");
    return;
  }
  size_t needed = static_cast<size_t>(width_ * height_);
  if (buffer_.size() < needed) {
    updateSize();
    if (buffer_.size() < needed) return;
  }
  if (prevBuffer_.size() != needed) {
    prevBuffer_.assign(needed, {});
    hasPrev_ = false;
  }
  bool fullRedraw = !hasPrev_ || alwaysFullRedraw_;
  std::wstring& out = drawBuffer_;
  out.clear();
  out.reserve(static_cast<size_t>(width_ * height_ * 2 + height_ * 32));

  if (fullRedraw) {
    if (clearOnNextFullRedraw_) {
      out.append(L"\x1b[2J");
    }
    Color curFg{};
    Color curBg{};
    bool hasColor = false;

    for (int y = 0; y < height_; ++y) {
      appendCursorPos(out, 1, y + 1);
      int rowStart = y * width_;
      for (int x = 0; x < width_; ++x) {
        const Cell& cell = buffer_[rowStart + x];
        if (cell.continuation) continue;
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
        appendCellGlyph(out, cell);
      }
    }
    out.append(L"\x1b[0m");
    if (!writeOutput(out)) {
      reportWriteError(L"draw");
      return;
    }
    prevBuffer_ = buffer_;
    hasPrev_ = true;
    clearOnNextFullRedraw_ = false;
    return;
  }

  bool wrote = false;
  Color curFg{};
  Color curBg{};
  bool hasColor = false;
  for (int y = 0; y < height_; ++y) {
    size_t rowStart = static_cast<size_t>(y) * width_;
    int x = 0;
    while (x < width_) {
      while (x < width_ &&
             sameCell(buffer_[rowStart + x], prevBuffer_[rowStart + x])) {
        ++x;
      }
      if (x >= width_) break;
      int spanStart = x;
      while (x < width_ &&
             !sameCell(buffer_[rowStart + x], prevBuffer_[rowStart + x])) {
        ++x;
      }
      int spanEnd = x;
      while (spanStart > 0 && buffer_[rowStart + spanStart].continuation) {
        --spanStart;
      }
      while (spanEnd < width_ && buffer_[rowStart + spanEnd].continuation) {
        ++spanEnd;
      }
      appendCursorPos(out, spanStart + 1, y + 1);

      for (int i = spanStart; i < spanEnd; ++i) {
        const Cell& cell = buffer_[rowStart + i];
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
        appendCellGlyph(out, cell);
        prevBuffer_[rowStart + i] = cell;
      }
      wrote = true;
    }
  }
  if (wrote) {
    out.append(L"\x1b[0m");
    if (!writeOutput(out)) {
      reportWriteError(L"draw");
      return;
    }
  }
}
